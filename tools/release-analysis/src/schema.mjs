import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import Ajv2020 from "ajv/dist/2020.js";
import addFormats from "ajv-formats";
import { ReleaseAnalysisError } from "./errors.mjs";

const SCHEMA_NAMES = [
  "release-analysis.schema.json",
  "base-state-input.schema.json",
  "base-state-output.schema.json",
  "candidate-analysis-input.schema.json",
  "candidate-analysis-output.schema.json",
  "predecessor-check-input.schema.json",
  "predecessor-check-output.schema.json",
  "error-output.schema.json",
];

const ajv = new Ajv2020({ allErrors: true, strict: true });
addFormats(ajv);

const schemas = new Map();
for (const name of SCHEMA_NAMES) {
  const url = new URL(`../schemas/${name}`, import.meta.url);
  const schema = JSON.parse(readFileSync(fileURLToPath(url), "utf8"));
  schemas.set(name, schema);
  ajv.addSchema(schema);
}

const validators = new Map(
  [...schemas].map(([name, schema]) => [name, ajv.getSchema(schema.$id)]),
);

export function validateSchema(name, value) {
  const validate = validators.get(name);
  if (!validate) {
    throw new Error(`Unknown schema: ${name}`);
  }
  if (validate(value)) {
    return value;
  }

  const details = (validate.errors ?? []).map((error) => ({
    path: error.instancePath || "/",
    keyword: error.keyword,
    message: error.message ?? "schema validation failed",
  }));
  throw new ReleaseAnalysisError(
    "INVALID_INPUT",
    `Input does not match ${name}`,
    details,
  );
}

export function assertOutputSchema(name, value) {
  const validate = validators.get(name);
  if (!validate?.(value)) {
    throw new Error(
      `Generated output failed ${name}: ${JSON.stringify(validate?.errors ?? [])}`,
    );
  }
  return value;
}
