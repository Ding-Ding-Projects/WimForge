"use strict";

const MAX_PATTERN_LENGTH = 512;
const MAX_DOCUMENTS = 2500;
const MAX_FIELD_LENGTH = 250000;
const MAX_RESULTS = 200;
const MAX_TEST_MATCHES = 500;
const ALLOWED_FLAGS = new Set(["i", "m", "s", "u"]);
const ALLOWED_FIELDS = new Set(["pageTitle", "heading", "content", "path"]);

function validateRequest(pattern, flags) {
  if (typeof pattern !== "string" || pattern.length === 0) {
    throw new Error("Enter a regular expression.");
  }
  if (pattern.length > MAX_PATTERN_LENGTH) {
    throw new Error(`Patterns are limited to ${MAX_PATTERN_LENGTH} characters.`);
  }
  if (!Array.isArray(flags) || flags.some((flag) => !ALLOWED_FLAGS.has(flag))) {
    throw new Error("The regex project contains an unsupported flag.");
  }
  return new RegExp(pattern, [...new Set(flags)].join(""));
}

function matchField(expression, value) {
  if (typeof value !== "string") return null;
  const boundedValue = value.slice(0, MAX_FIELD_LENGTH);
  expression.lastIndex = 0;
  const match = expression.exec(boundedValue);
  if (!match) return null;
  return { index: match.index, length: match[0].length };
}

function searchDocuments(message) {
  const expression = validateRequest(message.pattern, message.flags);
  const fields = Array.isArray(message.fields)
    ? message.fields.filter((field) => ALLOWED_FIELDS.has(field))
    : [];
  if (fields.length === 0) throw new Error("Select at least one search field.");

  const documents = Array.isArray(message.documents)
    ? message.documents.slice(0, MAX_DOCUMENTS)
    : [];
  const results = [];
  let total = 0;

  documents.forEach((document, documentIndex) => {
    let firstMatch = null;
    for (const field of fields) {
      const match = matchField(expression, document[field]);
      if (match) {
        firstMatch = { field, ...match };
        break;
      }
    }
    if (!firstMatch) return;
    total += 1;
    if (results.length < MAX_RESULTS) {
      results.push({ documentIndex, ...firstMatch });
    }
  });

  return { type: "search", results, total, limited: total > results.length };
}

function testPattern(message) {
  const baseExpression = validateRequest(message.pattern, message.flags);
  const flags = baseExpression.flags.includes("g")
    ? baseExpression.flags
    : `${baseExpression.flags}g`;
  const expression = new RegExp(baseExpression.source, flags);
  const unicodeMode = expression.flags.includes("u");
  const text = typeof message.text === "string" ? message.text.slice(0, 50000) : "";
  const matches = [];
  let match;

  while ((match = expression.exec(text)) && matches.length < MAX_TEST_MATCHES) {
    matches.push({ index: match.index, length: match[0].length });
    if (match[0].length === 0) {
      const codePoint = text.codePointAt(expression.lastIndex);
      expression.lastIndex += unicodeMode && codePoint !== undefined && codePoint > 0xffff ? 2 : 1;
    }
  }

  return {
    type: "test",
    matches,
    limited: matches.length === MAX_TEST_MATCHES,
  };
}

self.addEventListener("message", (event) => {
  const message = event.data || {};
  try {
    const payload = message.type === "test"
      ? testPattern(message)
      : searchDocuments(message);
    self.postMessage({ requestId: message.requestId, ok: true, ...payload });
  } catch (error) {
    self.postMessage({
      requestId: message.requestId,
      ok: false,
      error: error instanceof Error ? error.message : String(error),
    });
  }
});
