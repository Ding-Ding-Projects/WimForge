---
title: Wiki search
description: Search every WimForge wiki page with plain text or a bounded regular expression, and build reusable regex projects.
hide:
  - toc
---

# Search the WimForge wiki

This search is limited to the complete wiki published on this site. Plain mode
uses case-insensitive AND matching; regex mode runs in an isolated worker with
length, result, and time limits so an expensive expression cannot lock the page.
The search in the site header remains available for the whole documentation set.

<noscript>
JavaScript is required for the dedicated regex search. Every wiki page remains
available from the <a href="../wiki/Home/">wiki home</a>, and the site-header search still works.
</noscript>

<section class="wf-wiki-search" id="wf-wiki-search" aria-labelledby="wf-wiki-search-heading">
  <div class="wf-search-card">
    <div class="wf-search-card__heading">
      <div>
        <p class="wf-search-kicker">LOCAL · PRIVATE · COMPLETE WIKI</p>
        <h2 id="wf-wiki-search-heading">Find anything in WimForge</h2>
      </div>
      <button class="md-button" id="wf-open-regex-builder" type="button">
        Regex builder
      </button>
    </div>

    <div class="wf-search-controls">
      <label class="wf-field wf-field--grow" for="wf-search-query">
        <span>Search query</span>
        <input id="wf-search-query" type="search" autocomplete="off" maxlength="512"
               spellcheck="false" placeholder="Try: recovery history or mount.*discard">
      </label>

      <label class="wf-field" for="wf-search-mode">
        <span>Mode</span>
        <select id="wf-search-mode">
          <option value="plain">Plain text</option>
          <option value="regex">Regular expression</option>
        </select>
      </label>

      <button class="md-button md-button--primary wf-search-submit" id="wf-run-wiki-search" type="button">
        Search wiki
      </button>
    </div>

    <fieldset class="wf-search-scope" id="wf-search-scope">
      <legend>Search in</legend>
      <label><input type="checkbox" value="pageTitle" checked> Page titles</label>
      <label><input type="checkbox" value="heading" checked> Headings</label>
      <label><input type="checkbox" value="content" checked> Page content</label>
      <label><input type="checkbox" value="path"> URL paths</label>
    </fieldset>

    <div class="wf-search-feedback" id="wf-search-feedback" role="status" aria-live="polite">
      Loading the wiki index…
    </div>
  </div>

  <div class="wf-search-results" id="wf-search-results" role="region" aria-label="Wiki search results"></div>
</section>

<div class="wf-dialog-host">
<dialog class="wf-regex-dialog" id="wf-regex-dialog" aria-labelledby="wf-regex-dialog-title"
        aria-describedby="wf-regex-dialog-description">
  <div class="wf-regex-dialog__surface">
    <header class="wf-regex-dialog__header">
      <div>
        <p class="wf-search-kicker">BUILD · TEST · REUSE</p>
        <h2 id="wf-regex-dialog-title">Regex builder</h2>
        <p id="wf-regex-dialog-description">Build a JavaScript-compatible expression, test it locally, then use it across the wiki.</p>
      </div>
      <button class="wf-icon-button" id="wf-close-regex-builder" type="button" aria-label="Close regex builder">×</button>
    </header>

    <div class="wf-regex-dialog__body">
      <section class="wf-builder-section" aria-labelledby="wf-builder-project-heading">
        <div class="wf-builder-section__heading">
          <h3 id="wf-builder-project-heading">Builder project</h3>
          <div class="wf-builder-actions">
            <input id="wf-regex-import-file" type="file" accept="application/json,.json" hidden>
            <button class="md-button" id="wf-import-regex" type="button">Import JSON</button>
            <button class="md-button" id="wf-export-regex" type="button">Export JSON</button>
          </div>
        </div>

        <div class="wf-builder-grid wf-builder-grid--meta">
          <label class="wf-field" for="wf-regex-name">
            <span>Name</span>
            <input id="wf-regex-name" type="text" maxlength="80" placeholder="Recovery checks">
          </label>
          <label class="wf-field" for="wf-regex-description">
            <span>Description</span>
            <input id="wf-regex-description" type="text" maxlength="240" placeholder="What this expression finds">
          </label>
        </div>
        <p class="wf-builder-message" id="wf-import-export-feedback" role="status" aria-live="polite"></p>
        <details class="wf-paste-import">
          <summary>Paste project JSON</summary>
          <label class="wf-field" for="wf-regex-import-json">
            <span>Regex project JSON</span>
            <textarea id="wf-regex-import-json" rows="4" maxlength="262144" spellcheck="false"
                      placeholder='{"format":"wimforge-wiki-regex","version":1,…}'></textarea>
          </label>
          <button class="md-button" id="wf-import-pasted-regex" type="button">Import pasted JSON</button>
        </details>
      </section>

      <section class="wf-builder-section" aria-labelledby="wf-builder-pattern-heading">
        <div class="wf-builder-section__heading">
          <h3 id="wf-builder-pattern-heading">Expression</h3>
          <button class="md-button" id="wf-copy-regex" type="button">Copy pattern</button>
        </div>

        <label class="wf-field" for="wf-regex-pattern">
          <span>Pattern <small>maximum 512 characters</small></span>
          <textarea id="wf-regex-pattern" rows="3" maxlength="512" spellcheck="false"
                    placeholder="(?:mount|unmount).*?(?:recover|discard)"></textarea>
        </label>

        <div class="wf-builder-grid">
          <fieldset class="wf-builder-flags" id="wf-regex-flags">
            <legend>Flags</legend>
            <label><input type="checkbox" value="i" checked> <code>i</code> Ignore case</label>
            <label><input type="checkbox" value="m"> <code>m</code> Multiline anchors</label>
            <label><input type="checkbox" value="s"> <code>s</code> Dot matches newline</label>
            <label><input type="checkbox" value="u" checked> <code>u</code> Unicode</label>
          </fieldset>

          <fieldset class="wf-builder-flags" id="wf-regex-builder-scope">
            <legend>Wiki fields</legend>
            <label><input type="checkbox" value="pageTitle" checked> Page titles</label>
            <label><input type="checkbox" value="heading" checked> Headings</label>
            <label><input type="checkbox" value="content" checked> Page content</label>
            <label><input type="checkbox" value="path"> URL paths</label>
          </fieldset>
        </div>

        <div class="wf-field wf-literal-field">
          <span>Escaped literal</span>
          <div class="wf-inline-control">
            <input id="wf-regex-literal" type="text" placeholder="Text such as project.json">
            <button class="md-button" id="wf-insert-literal" type="button">Insert</button>
          </div>
          <small>Regex punctuation is escaped before insertion.</small>
        </div>

        <div class="wf-builder-validation" id="wf-regex-validation" role="status" aria-live="polite">
          Enter a pattern to begin.
        </div>
      </section>

      <section class="wf-builder-section" aria-labelledby="wf-builder-pieces-heading">
        <div class="wf-builder-section__heading">
          <h3 id="wf-builder-pieces-heading">Pattern pieces</h3>
          <button class="md-button" id="wf-clear-regex" type="button">Clear pattern</button>
        </div>

        <div class="wf-token-groups">
          <div class="wf-token-group">
            <h4>Characters</h4>
            <div class="wf-token-list">
              <button type="button" data-regex-template="."><code>.</code><span>Any character</span></button>
              <button type="button" data-regex-template="\d"><code>\d</code><span>Digit</span></button>
              <button type="button" data-regex-template="\D"><code>\D</code><span>Not a digit</span></button>
              <button type="button" data-regex-template="\w"><code>\w</code><span>Word character</span></button>
              <button type="button" data-regex-template="\W"><code>\W</code><span>Not a word character</span></button>
              <button type="button" data-regex-template="\s"><code>\s</code><span>Whitespace</span></button>
              <button type="button" data-regex-template="\S"><code>\S</code><span>Not whitespace</span></button>
              <button type="button" data-regex-template="\p{L}"><code>\p{L}</code><span>Unicode letter</span></button>
              <button type="button" data-regex-template="[abc]" data-regex-select="1:4"><code>[abc]</code><span>Character set</span></button>
              <button type="button" data-regex-template="[^abc]" data-regex-select="2:5"><code>[^abc]</code><span>Not in set</span></button>
            </div>
          </div>

          <div class="wf-token-group">
            <h4>Boundaries</h4>
            <div class="wf-token-list">
              <button type="button" data-regex-template="^"><code>^</code><span>Start</span></button>
              <button type="button" data-regex-template="$"><code>$</code><span>End</span></button>
              <button type="button" data-regex-template="\b"><code>\b</code><span>Word boundary</span></button>
              <button type="button" data-regex-template="\B"><code>\B</code><span>Not a boundary</span></button>
            </div>
          </div>

          <div class="wf-token-group">
            <h4>Groups and choices</h4>
            <div class="wf-token-list">
              <button type="button" data-regex-template="({selection})" data-regex-cursor="1"><code>(…)</code><span>Capture</span></button>
              <button type="button" data-regex-template="(?:{selection})" data-regex-cursor="3"><code>(?:…)</code><span>Non-capture</span></button>
              <button type="button" data-regex-template="(?&lt;name&gt;{selection})" data-regex-cursor="8"><code>(?&lt;name&gt;…)</code><span>Named capture</span></button>
              <button type="button" data-regex-template="|"><code>|</code><span>Either / or</span></button>
              <button type="button" data-regex-template="(?={selection})" data-regex-cursor="3"><code>(?=…)</code><span>Look ahead</span></button>
              <button type="button" data-regex-template="(?!{selection})" data-regex-cursor="3"><code>(?!…)</code><span>Negative look ahead</span></button>
              <button type="button" data-regex-template="(?&lt;={selection})" data-regex-cursor="4"><code>(?&lt;=…)</code><span>Look behind</span></button>
              <button type="button" data-regex-template="(?&lt;!{selection})" data-regex-cursor="4"><code>(?&lt;!…)</code><span>Negative look behind</span></button>
              <button type="button" data-regex-template="\1"><code>\1</code><span>Capture reference</span></button>
              <button type="button" data-regex-template="\k&lt;name&gt;"><code>\k&lt;name&gt;</code><span>Named reference</span></button>
            </div>
          </div>

          <div class="wf-token-group">
            <h4>Repetition</h4>
            <div class="wf-token-list">
              <button type="button" data-regex-template="?"><code>?</code><span>Zero or one</span></button>
              <button type="button" data-regex-template="*"><code>*</code><span>Zero or more</span></button>
              <button type="button" data-regex-template="+"><code>+</code><span>One or more</span></button>
              <button type="button" data-regex-template="{n}" data-regex-select="1:2"><code>{n}</code><span>Exactly n</span></button>
              <button type="button" data-regex-template="{n,}" data-regex-select="1:2"><code>{n,}</code><span>At least n</span></button>
              <button type="button" data-regex-template="{n,m}" data-regex-select="1:4"><code>{n,m}</code><span>Between n and m</span></button>
              <button type="button" data-regex-template="?" title="Append after a quantifier"><code>*?</code><span>Make lazy</span></button>
            </div>
          </div>

          <div class="wf-token-group">
            <h4>Common recipes</h4>
            <div class="wf-token-list">
              <button type="button" data-regex-template="\b(?:{selection})\b" data-regex-cursor="5"><code>\b…\b</code><span>Whole word</span></button>
              <button type="button" data-regex-template="^(?:{selection})" data-regex-cursor="4"><code>^…</code><span>Starts with</span></button>
              <button type="button" data-regex-template="(?:{selection})$" data-regex-cursor="3"><code>…$</code><span>Ends with</span></button>
              <button type="button" data-regex-template="^.*(?:{selection}).*$" data-regex-cursor="6"><code>^.*….*$</code><span>Line containing</span></button>
              <button type="button" data-regex-template="(?:first|second)" data-regex-select="3:15"><code>a|b</code><span>Either phrase</span></button>
              <button type="button" data-regex-template="^(?=.*first)(?=.*second).*$" data-regex-select="6:11"><code>(?=.*a)…</code><span>Contains both</span></button>
            </div>
          </div>
        </div>
      </section>

      <section class="wf-builder-section" aria-labelledby="wf-builder-test-heading">
        <div class="wf-builder-section__heading">
          <h3 id="wf-builder-test-heading">Live test</h3>
          <span class="wf-builder-match-count" id="wf-regex-test-count">0 matches</span>
        </div>
        <label class="wf-field" for="wf-regex-test-text">
          <span>Sample text <small>maximum 50,000 characters</small></span>
          <textarea id="wf-regex-test-text" rows="6" maxlength="50000" spellcheck="false"
                    placeholder="Paste representative wiki text here. It stays in this browser."></textarea>
        </label>
        <pre class="wf-regex-preview" id="wf-regex-preview" aria-label="Highlighted regex matches">Matches appear here.</pre>
      </section>
    </div>

    <footer class="wf-regex-dialog__footer">
      <p>Projects are JSON files; imported patterns still pass the same validation and execution limits.</p>
      <div>
        <button class="md-button" id="wf-cancel-regex-builder" type="button">Cancel</button>
        <button class="md-button md-button--primary" id="wf-use-regex" type="button">Use in wiki search</button>
      </div>
    </footer>
  </div>
</dialog>
</div>

## Search behavior and limits

- Plain text splits the query on whitespace and requires every term to appear
  in at least one selected field. Queries are limited to 512 characters and
  64 plain-text terms.
- Regex uses the browser's JavaScript regular-expression syntax. Expressions
  are limited to 512 characters, 200 results, and a short worker deadline.
- Search and builder data stay in the browser. Nothing is uploaded.
- Builder exports use the published
  [WimForge regex-project JSON schema](assets/wiki-regex-builder.schema.json).
