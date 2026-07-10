(() => {
  "use strict";

  const PROJECT_FORMAT = "wimforge-wiki-regex";
  const PROJECT_VERSION = 1;
  const MAX_IMPORT_BYTES = 256 * 1024;
  const REGEX_TIMEOUT_MS = 900;
  const TEST_TIMEOUT_MS = 600;
  const RESULT_LIMIT = 200;
  const MAX_QUERY_LENGTH = 512;
  const MAX_PLAIN_TOKENS = 64;
  const VALID_FLAGS = ["i", "m", "s", "u"];
  const VALID_SCOPES = ["pageTitle", "heading", "content", "path"];
  const scriptUrl = [...document.scripts]
    .map((script) => script.src)
    .find((src) => /\/wiki-search\.js(?:\?|$)/.test(src));
  const workerUrl = scriptUrl
    ? new URL("wiki-regex-worker.js", scriptUrl).href
    : "../javascripts/wiki-regex-worker.js";
  let wikiIndexPromise = null;

  const loadWikiIndex = (indexUrl) => {
    if (!wikiIndexPromise) {
      wikiIndexPromise = fetch(indexUrl)
        .then((response) => {
          if (!response.ok) throw new Error(`Wiki index request failed (${response.status}).`);
          return response.json();
        })
        .catch((error) => {
          wikiIndexPromise = null;
          throw error;
        });
    }
    return wikiIndexPromise;
  };

  const debounce = (callback, delay) => {
    let timer = 0;
    return (...args) => {
      window.clearTimeout(timer);
      timer = window.setTimeout(() => callback(...args), delay);
    };
  };

  const plainText = (html) => {
    const template = document.createElement("template");
    template.innerHTML = html || "";
    return (template.content.textContent || "").replace(/\s+/g, " ").trim();
  };

  const checkedValues = (container) => [...container.querySelectorAll("input:checked")]
    .map((input) => input.value);

  const setCheckedValues = (container, values) => {
    const allowed = new Set(values);
    container.querySelectorAll("input[type='checkbox']").forEach((input) => {
      input.checked = allowed.has(input.value);
    });
  };

  const pluralize = (count, singular, plural = `${singular}s`) =>
    `${count} ${count === 1 ? singular : plural}`;

  function siteRootUrl() {
    const configElement = document.getElementById("__config");
    const config = configElement ? JSON.parse(configElement.textContent) : { base: "." };
    return new URL(`${config.base || "."}/`, window.location.href);
  }

  function buildWikiDocuments(index) {
    const entries = Array.isArray(index.docs) ? index.docs : [];
    const wikiEntries = entries.filter((entry) =>
      typeof entry.location === "string" && /^wiki\/[^#]/i.test(entry.location));
    const pageTitles = new Map();

    wikiEntries.forEach((entry) => {
      const path = entry.location.split("#", 1)[0];
      if (!pageTitles.has(path) || !entry.location.includes("#")) {
        pageTitles.set(path, entry.title || path);
      }
    });

    return wikiEntries.map((entry) => {
      const path = entry.location.split("#", 1)[0];
      return {
        location: entry.location,
        path,
        pageTitle: pageTitles.get(path) || entry.title || path,
        heading: entry.title || pageTitles.get(path) || path,
        content: plainText(entry.text),
      };
    });
  }

  function snippet(value, matchIndex = -1, matchLength = 0) {
    const text = (value || "").replace(/\s+/g, " ").trim();
    if (!text) return { text: "No summary is available for this heading.", start: -1, length: 0 };
    if (matchIndex < 0) return { text: text.slice(0, 280), start: -1, length: 0 };
    const windowStart = Math.max(0, matchIndex - 90);
    const windowEnd = Math.min(text.length, Math.max(matchIndex + matchLength + 120, windowStart + 240));
    const prefix = windowStart > 0 ? "…" : "";
    const suffix = windowEnd < text.length ? "…" : "";
    return {
      text: `${prefix}${text.slice(windowStart, windowEnd)}${suffix}`,
      start: prefix.length + matchIndex - windowStart,
      length: matchLength,
    };
  }

  function appendHighlighted(container, value, start, length) {
    if (start < 0 || start > value.length) {
      container.textContent = value;
      return;
    }
    container.append(document.createTextNode(value.slice(0, start)));
    const mark = document.createElement("mark");
    mark.textContent = length === 0 ? "∅" : value.slice(start, start + length);
    if (length === 0) mark.title = "Zero-length match";
    container.append(mark, document.createTextNode(value.slice(start + length)));
  }

  function validateProject(project) {
    if (!project || typeof project !== "object" || Array.isArray(project)) {
      throw new Error("The selected file is not a regex project object.");
    }
    if (project.format !== PROJECT_FORMAT || project.version !== PROJECT_VERSION) {
      throw new Error("This is not a supported WimForge wiki regex project.");
    }
    const allowedKeys = new Set([
      "$schema", "format", "version", "name", "description", "pattern", "flags", "scope", "testText",
    ]);
    const unknownKey = Object.keys(project).find((key) => !allowedKeys.has(key));
    if (unknownKey) throw new Error(`Project field '${unknownKey}' is not supported.`);
    if ("$schema" in project && typeof project.$schema !== "string") {
      throw new Error("Project field '$schema' must be a string.");
    }
    const stringLimits = { name: 80, description: 240, pattern: 512, testText: 50000 };
    Object.entries(stringLimits).forEach(([key, maximum]) => {
      if (typeof project[key] !== "string" || project[key].length > maximum) {
        throw new Error(`Project field '${key}' is missing or too long.`);
      }
    });
    if (!Array.isArray(project.flags) || project.flags.some((flag) => !VALID_FLAGS.includes(flag))) {
      throw new Error("The project contains an unsupported regex flag.");
    }
    if (new Set(project.flags).size !== project.flags.length) {
      throw new Error("The project contains duplicate regex flags.");
    }
    if (!Array.isArray(project.scope) || project.scope.length === 0
        || project.scope.some((field) => !VALID_SCOPES.includes(field))) {
      throw new Error("The project must contain at least one supported search field.");
    }
    if (new Set(project.scope).size !== project.scope.length) {
      throw new Error("The project contains duplicate search fields.");
    }
    return {
      name: project.name,
      description: project.description,
      pattern: project.pattern,
      flags: [...new Set(project.flags)],
      scope: [...new Set(project.scope)],
      testText: project.testText,
    };
  }

  function initWikiSearch() {
    const root = document.getElementById("wf-wiki-search");
    if (!root || root.dataset.initialized === "true") return;
    root.dataset.initialized = "true";

    const queryInput = document.getElementById("wf-search-query");
    const modeSelect = document.getElementById("wf-search-mode");
    const searchButton = document.getElementById("wf-run-wiki-search");
    const searchScope = document.getElementById("wf-search-scope");
    const feedback = document.getElementById("wf-search-feedback");
    const resultsElement = document.getElementById("wf-search-results");
    const dialog = document.getElementById("wf-regex-dialog");
    const openBuilderButton = document.getElementById("wf-open-regex-builder");
    const closeBuilderButton = document.getElementById("wf-close-regex-builder");
    const cancelBuilderButton = document.getElementById("wf-cancel-regex-builder");
    const useRegexButton = document.getElementById("wf-use-regex");
    const patternInput = document.getElementById("wf-regex-pattern");
    const flagsElement = document.getElementById("wf-regex-flags");
    const builderScope = document.getElementById("wf-regex-builder-scope");
    const validationElement = document.getElementById("wf-regex-validation");
    const testTextInput = document.getElementById("wf-regex-test-text");
    const testCount = document.getElementById("wf-regex-test-count");
    const previewElement = document.getElementById("wf-regex-preview");
    const importFileInput = document.getElementById("wf-regex-import-file");
    const importExportFeedback = document.getElementById("wf-import-export-feedback");
    const nameInput = document.getElementById("wf-regex-name");
    const descriptionInput = document.getElementById("wf-regex-description");
    const rootUrl = siteRootUrl();
    const indexUrl = new URL("search/search_index.json", rootUrl);
    let documents = [];
    let searchWorker = null;
    let testWorker = null;
    let searchRequestId = 0;
    let testRequestId = 0;
    let activeRegexFlags = ["i", "u"];
    let patternSelection = { start: 0, end: 0 };

    const rememberPatternSelection = () => {
      patternSelection = {
        start: patternInput.selectionStart,
        end: patternInput.selectionEnd,
      };
    };

    const builderProject = () => ({
      $schema: new URL("assets/wiki-regex-builder.schema.json", rootUrl).href,
      format: PROJECT_FORMAT,
      version: PROJECT_VERSION,
      name: nameInput.value.trim(),
      description: descriptionInput.value.trim(),
      pattern: patternInput.value,
      flags: checkedValues(flagsElement),
      scope: checkedValues(builderScope),
      testText: testTextInput.value,
    });

    const saveBuilderProject = () => {
      try {
        window.localStorage.setItem("wimforge.wiki.regexProject.v1", JSON.stringify(builderProject()));
      } catch (_) {
        // The builder remains fully usable when storage is unavailable.
      }
    };

    const applyBuilderProject = (project) => {
      nameInput.value = project.name;
      descriptionInput.value = project.description;
      patternInput.value = project.pattern;
      setCheckedValues(flagsElement, project.flags);
      setCheckedValues(builderScope, project.scope);
      testTextInput.value = project.testText;
      saveBuilderProject();
      validateAndTest();
    };

    const importProjectText = (text, label) => {
      if (new Blob([text]).size > MAX_IMPORT_BYTES) {
        throw new Error("Project files are limited to 256 KiB.");
      }
      const project = validateProject(JSON.parse(text));
      applyBuilderProject(project);
      importExportFeedback.textContent = `Imported '${project.name || label}'.`;
      return project;
    };

    const updateUrl = () => {
      const url = new URL(window.location.href);
      const query = queryInput.value.trim();
      if (query) url.searchParams.set("wiki_q", query); else url.searchParams.delete("wiki_q");
      if (modeSelect.value === "regex") {
        url.searchParams.set("wiki_mode", "regex");
        url.searchParams.set("wiki_flags", activeRegexFlags.join(""));
      } else {
        url.searchParams.delete("wiki_mode");
        url.searchParams.delete("wiki_flags");
      }
      url.searchParams.set("wiki_scope", checkedValues(searchScope).join(","));
      window.history.replaceState(null, "", url);
    };

    const renderResults = (matches, total, mode) => {
      resultsElement.replaceChildren();
      if (!matches.length) {
        const empty = document.createElement("div");
        empty.className = "wf-search-empty";
        const heading = document.createElement("h3");
        heading.textContent = queryInput.value.trim()
          ? "No wiki sections matched"
          : "The full wiki is ready";
        const copy = document.createElement("p");
        copy.textContent = queryInput.value.trim()
          ? "Try a broader query, another field, or use the builder to test the expression."
          : "Enter a query above, or open the wiki home to browse every page.";
        empty.append(heading, copy);
        resultsElement.append(empty);
        return;
      }

      const list = document.createElement("ol");
      list.className = "wf-search-result-list";
      matches.forEach((match) => {
        const documentEntry = documents[match.documentIndex];
        if (!documentEntry) return;
        const item = document.createElement("li");
        const card = document.createElement("article");
        card.className = "wf-search-result";
        const link = document.createElement("a");
        link.href = new URL(documentEntry.location, rootUrl).href;
        link.className = "wf-search-result__title";
        link.textContent = documentEntry.heading;
        const page = document.createElement("div");
        page.className = "wf-search-result__page";
        page.textContent = documentEntry.pageTitle === documentEntry.heading
          ? documentEntry.path.replace(/^wiki\//i, "Wiki / ").replace(/\/$/, "")
          : documentEntry.pageTitle;
        const fieldValue = documentEntry[match.field] || documentEntry.content;
        const excerpt = snippet(fieldValue, match.index, match.length);
        const summary = document.createElement("p");
        appendHighlighted(summary, excerpt.text, excerpt.start, excerpt.length);
        const path = document.createElement("code");
        path.className = "wf-search-result__path";
        path.textContent = documentEntry.location;
        card.append(link, page, summary, path);
        item.append(card);
        list.append(item);
      });
      resultsElement.append(list);
      const shown = matches.length;
      feedback.textContent = total > shown
        ? `${pluralize(total, "matching section")}; showing the first ${shown}.`
        : `${pluralize(total, "matching section")} in ${mode === "regex" ? "regex" : "plain-text"} mode.`;
    };

    const runPlainSearch = (query, fields) => {
      const tokens = query.toLocaleLowerCase().split(/\s+/).filter(Boolean);
      const matches = [];
      documents.forEach((documentEntry, documentIndex) => {
        const values = fields.map((field) => documentEntry[field] || "");
        const normalizedValues = values.map((value) => value.toLocaleLowerCase());
        if (!tokens.every((token) => normalizedValues.some((value) => value.includes(token)))) return;
        let fieldIndex = normalizedValues.findIndex((value) => value.includes(tokens[0] || ""));
        if (fieldIndex < 0) fieldIndex = 0;
        const index = tokens.length ? normalizedValues[fieldIndex].indexOf(tokens[0]) : -1;
        matches.push({
          documentIndex,
          field: fields[fieldIndex],
          index,
          length: tokens[0] ? tokens[0].length : 0,
        });
      });
      renderResults(matches.slice(0, RESULT_LIMIT), matches.length, "plain");
    };

    const runWorker = (kind, payload, timeout) => new Promise((resolve, reject) => {
      const worker = new Worker(workerUrl);
      if (kind === "search") {
        if (searchWorker) searchWorker.terminate();
        searchWorker = worker;
      } else {
        if (testWorker) testWorker.terminate();
        testWorker = worker;
      }
      const timer = window.setTimeout(() => {
        worker.terminate();
        reject(new Error("The expression exceeded the safe execution time. Try a more specific pattern."));
      }, timeout);
      worker.addEventListener("message", (event) => {
        window.clearTimeout(timer);
        worker.terminate();
        if (event.data && event.data.ok) resolve(event.data);
        else reject(new Error(event.data?.error || "The regular expression could not be evaluated."));
      }, { once: true });
      worker.addEventListener("error", () => {
        window.clearTimeout(timer);
        worker.terminate();
        reject(new Error("The isolated regex worker could not start."));
      }, { once: true });
      worker.postMessage(payload);
    });

    const runSearch = async () => {
      const query = queryInput.value.trim();
      const fields = checkedValues(searchScope);
      const requestId = ++searchRequestId;
      if (searchWorker) {
        searchWorker.terminate();
        searchWorker = null;
      }
      updateUrl();
      if (!documents.length) {
        feedback.textContent = "The wiki index is still loading.";
        return;
      }
      if (!query) {
        feedback.textContent = "Enter a query to search every wiki page.";
        renderResults([], 0, modeSelect.value);
        return;
      }
      if (query.length > MAX_QUERY_LENGTH) {
        feedback.textContent = `Queries are limited to ${MAX_QUERY_LENGTH} characters.`;
        renderResults([], 0, modeSelect.value);
        return;
      }
      if (!fields.length) {
        feedback.textContent = "Select at least one search field.";
        renderResults([], 0, modeSelect.value);
        return;
      }
      if (modeSelect.value === "plain") {
        if (searchWorker) searchWorker.terminate();
        if (query.split(/\s+/).filter(Boolean).length > MAX_PLAIN_TOKENS) {
          feedback.textContent = `Plain-text search is limited to ${MAX_PLAIN_TOKENS} terms.`;
          renderResults([], 0, "plain");
          return;
        }
        runPlainSearch(query, fields);
        return;
      }

      feedback.textContent = "Running the regular expression safely…";
      try {
        const response = await runWorker("search", {
          type: "search",
          requestId,
          pattern: query,
          flags: activeRegexFlags,
          fields,
          documents,
        }, REGEX_TIMEOUT_MS);
        if (requestId !== searchRequestId) return;
        renderResults(response.results, response.total, "regex");
      } catch (error) {
        if (requestId !== searchRequestId) return;
        feedback.textContent = error.message;
        resultsElement.replaceChildren();
      }
    };

    const renderTestPreview = (text, matches) => {
      previewElement.replaceChildren();
      if (!text) {
        previewElement.textContent = "Add sample text to see highlighted matches.";
        return;
      }
      if (!matches.length) {
        previewElement.textContent = text;
        return;
      }
      let cursor = 0;
      matches.forEach((match) => {
        previewElement.append(document.createTextNode(text.slice(cursor, match.index)));
        const mark = document.createElement("mark");
        mark.textContent = match.length === 0 ? "∅" : text.slice(match.index, match.index + match.length);
        if (match.length === 0) mark.title = "Zero-length match";
        previewElement.append(mark);
        cursor = match.index + match.length;
      });
      previewElement.append(document.createTextNode(text.slice(cursor)));
    };

    const validateAndTest = debounce(async () => {
      saveBuilderProject();
      const pattern = patternInput.value;
      const text = testTextInput.value;
      const requestId = ++testRequestId;
      if (!pattern) {
        if (testWorker) testWorker.terminate();
        validationElement.className = "wf-builder-validation";
        validationElement.textContent = "Enter a pattern to begin.";
        testCount.textContent = "0 matches";
        renderTestPreview(text, []);
        return;
      }
      validationElement.className = "wf-builder-validation wf-builder-validation--working";
      validationElement.textContent = "Validating in the isolated worker…";
      try {
        const response = await runWorker("test", {
          type: "test",
          requestId,
          pattern,
          flags: checkedValues(flagsElement),
          text,
        }, TEST_TIMEOUT_MS);
        if (requestId !== testRequestId) return;
        validationElement.className = "wf-builder-validation wf-builder-validation--valid";
        validationElement.textContent = response.limited
          ? "Valid expression. Preview stopped at the 500-match safety limit."
          : "Valid expression.";
        testCount.textContent = pluralize(response.matches.length, "match", "matches");
        renderTestPreview(text, response.matches);
      } catch (error) {
        if (requestId !== testRequestId) return;
        validationElement.className = "wf-builder-validation wf-builder-validation--invalid";
        validationElement.textContent = error.message;
        testCount.textContent = "Invalid pattern";
        renderTestPreview(text, []);
      }
    }, 180);

    const openBuilder = () => {
      if (modeSelect.value === "regex" && queryInput.value.trim()) {
        patternInput.value = queryInput.value.trim();
        setCheckedValues(flagsElement, activeRegexFlags);
        setCheckedValues(builderScope, checkedValues(searchScope));
      }
      const materialSearchToggle = document.getElementById("__search");
      if (materialSearchToggle?.checked) {
        materialSearchToggle.checked = false;
        materialSearchToggle.dispatchEvent(new Event("change", { bubbles: true }));
      }
      dialog.showModal();
      patternInput.focus();
      rememberPatternSelection();
      validateAndTest();
    };

    openBuilderButton.addEventListener("click", openBuilder);
    closeBuilderButton.addEventListener("click", () => dialog.close());
    cancelBuilderButton.addEventListener("click", () => dialog.close());
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) dialog.close();
    });
    dialog.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        useRegexButton.click();
      }
    });
    useRegexButton.addEventListener("click", () => {
      if (!patternInput.value) {
        validationElement.className = "wf-builder-validation wf-builder-validation--invalid";
        validationElement.textContent = "Enter a pattern before using it in search.";
        patternInput.focus();
        return;
      }
      if (!checkedValues(builderScope).length) {
        validationElement.className = "wf-builder-validation wf-builder-validation--invalid";
        validationElement.textContent = "Select at least one wiki field before searching.";
        builderScope.querySelector("input")?.focus();
        return;
      }
      queryInput.value = patternInput.value;
      modeSelect.value = "regex";
      activeRegexFlags = checkedValues(flagsElement);
      setCheckedValues(searchScope, checkedValues(builderScope));
      dialog.close();
      runSearch();
      queryInput.focus();
    });

    document.querySelectorAll("[data-regex-template]").forEach((button) => {
      button.addEventListener("pointerdown", (event) => event.preventDefault());
      button.addEventListener("click", () => {
        const { start, end } = patternSelection;
        const selected = patternInput.value.slice(start, end);
        const template = button.dataset.regexTemplate.replace("{selection}", selected);
        patternInput.setRangeText(template, start, end, "end");
        if (!selected && button.dataset.regexCursor) {
          const cursor = start + Number(button.dataset.regexCursor);
          patternInput.setSelectionRange(cursor, cursor);
        } else if (!selected && button.dataset.regexSelect) {
          const [selectionStart, selectionEnd] = button.dataset.regexSelect.split(":").map(Number);
          patternInput.setSelectionRange(start + selectionStart, start + selectionEnd);
        }
        patternInput.focus();
        rememberPatternSelection();
        validateAndTest();
      });
    });

    document.getElementById("wf-insert-literal").addEventListener("click", () => {
      const literalInput = document.getElementById("wf-regex-literal");
      if (!literalInput.value) {
        literalInput.focus();
        return;
      }
      const escaped = literalInput.value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      patternInput.setRangeText(escaped, patternSelection.start, patternSelection.end, "end");
      patternInput.focus();
      rememberPatternSelection();
      validateAndTest();
    });

    document.getElementById("wf-clear-regex").addEventListener("click", () => {
      patternInput.value = "";
      patternInput.focus();
      validateAndTest();
    });

    document.getElementById("wf-copy-regex").addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(patternInput.value);
        importExportFeedback.textContent = "Pattern copied to the clipboard.";
      } catch (_) {
        patternInput.select();
        importExportFeedback.textContent = "Clipboard access was unavailable; the pattern is selected for copying.";
      }
    });

    document.getElementById("wf-import-regex").addEventListener("click", () => importFileInput.click());
    importFileInput.addEventListener("change", async () => {
      const file = importFileInput.files?.[0];
      importFileInput.value = "";
      if (!file) return;
      if (file.size > MAX_IMPORT_BYTES) {
        importExportFeedback.textContent = "Import failed: project files are limited to 256 KiB.";
        return;
      }
      try {
        importProjectText(await file.text(), file.name);
      } catch (error) {
        importExportFeedback.textContent = `Import failed: ${error.message}`;
      }
    });

    document.getElementById("wf-import-pasted-regex").addEventListener("click", () => {
      const pastedJson = document.getElementById("wf-regex-import-json");
      if (!pastedJson.value.trim()) {
        importExportFeedback.textContent = "Import failed: paste a regex project first.";
        pastedJson.focus();
        return;
      }
      try {
        importProjectText(pastedJson.value, "pasted project");
      } catch (error) {
        importExportFeedback.textContent = `Import failed: ${error.message}`;
      }
    });

    document.getElementById("wf-export-regex").addEventListener("click", () => {
      try {
        const project = validateProject(builderProject());
        const blob = new Blob([`${JSON.stringify({
          $schema: new URL("assets/wiki-regex-builder.schema.json", rootUrl).href,
          format: PROJECT_FORMAT,
          version: PROJECT_VERSION,
          ...project,
        }, null, 2)}\n`], { type: "application/json" });
        const objectUrl = URL.createObjectURL(blob);
        const link = document.createElement("a");
        const baseName = (project.name || "wiki-regex")
          .toLocaleLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "wiki-regex";
        link.href = objectUrl;
        link.download = `${baseName}.json`;
        document.body.append(link);
        link.click();
        link.remove();
        window.setTimeout(() => URL.revokeObjectURL(objectUrl), 1000);
        importExportFeedback.textContent = `Exported '${link.download}'.`;
      } catch (error) {
        importExportFeedback.textContent = `Export failed: ${error.message}`;
      }
    });

    const delayedSearch = debounce(runSearch, 220);
    queryInput.addEventListener("input", delayedSearch);
    queryInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") runSearch();
    });
    searchButton.addEventListener("click", runSearch);
    modeSelect.addEventListener("change", () => {
      openBuilderButton.classList.toggle("wf-regex-active", modeSelect.value === "regex");
      runSearch();
    });
    searchScope.addEventListener("change", () => {
      runSearch();
    });
    patternInput.addEventListener("input", validateAndTest);
    ["focus", "select", "keyup", "mouseup", "input"].forEach((eventName) => {
      patternInput.addEventListener(eventName, rememberPatternSelection);
    });
    testTextInput.addEventListener("input", validateAndTest);
    flagsElement.addEventListener("change", validateAndTest);
    builderScope.addEventListener("change", saveBuilderProject);
    nameInput.addEventListener("input", saveBuilderProject);
    descriptionInput.addEventListener("input", saveBuilderProject);

    const parameters = new URL(window.location.href).searchParams;
    queryInput.value = parameters.get("wiki_q") || "";
    modeSelect.value = parameters.get("wiki_mode") === "regex" ? "regex" : "plain";
    openBuilderButton.classList.toggle("wf-regex-active", modeSelect.value === "regex");
    const parameterFlags = parameters.get("wiki_flags");
    if (parameterFlags !== null) {
      activeRegexFlags = [...new Set([...parameterFlags].filter((flag) => VALID_FLAGS.includes(flag)))];
      setCheckedValues(flagsElement, activeRegexFlags);
    }
    const parameterScope = parameters.get("wiki_scope")?.split(",").filter((field) => VALID_SCOPES.includes(field));
    if (parameterScope?.length) setCheckedValues(searchScope, parameterScope);

    if (!parameters.has("wiki_q")) {
      try {
        const savedProject = JSON.parse(window.localStorage.getItem("wimforge.wiki.regexProject.v1"));
        if (savedProject) applyBuilderProject(validateProject(savedProject));
      } catch (_) {
        // Ignore stale or unavailable local builder state.
      }
    }

    loadWikiIndex(indexUrl)
      .then((index) => {
        documents = buildWikiDocuments(index);
        const pageCount = new Set(documents.map((entry) => entry.path)).size;
        feedback.textContent = `${pluralize(pageCount, "wiki page")} indexed. Enter a query to search them all.`;
        if (queryInput.value.trim()) runSearch();
        else renderResults([], 0, modeSelect.value);
      })
      .catch((error) => {
        feedback.textContent = `The wiki index could not be loaded: ${error.message}`;
      });
  }

  if (typeof window.document$ !== "undefined") {
    window.document$.subscribe(initWikiSearch);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initWikiSearch, { once: true });
  } else {
    initWikiSearch();
  }
})();
