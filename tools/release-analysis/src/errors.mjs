export class ReleaseAnalysisError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = "ReleaseAnalysisError";
    this.code = code;
    this.details = details;
  }
}

export function structuredError(error) {
  const known = error instanceof ReleaseAnalysisError;
  return {
    schema_version: 1,
    ok: false,
    error: {
      code: known ? error.code : "INTERNAL_ERROR",
      message: known ? error.message : "Release analysis failed",
      details: known ? error.details : [],
    },
  };
}
