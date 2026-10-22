export type QueryResult = {
  ok: boolean;
  error: string;
  message: string;
  plan: string;
  elapsed_ms: number;
  rows_affected: number;
  columns: string[];
  types: string[];
  rows: string[][];
};

export type SchemaColumn = {
  name: string;
  type: string;
  length?: number;
  primary_key: boolean;
  not_null: boolean;
};

export type SchemaIndex = {
  name: string;
  unique: boolean;
  columns: string[];
};

export type SchemaTable = {
  name: string;
  columns: SchemaColumn[];
  indexes: SchemaIndex[];
  rows: number;
};

export type Session = {
  exec: (sql: string) => string;
  schema: () => string;
  reset: () => void;
  delete: () => void;
};

type KeelFactory = () => Promise<{ Database: new () => Session }>;

export async function openSession(): Promise<Session> {
  const load = new Function("return import('/keel/keel.js')") as () => Promise<{ default: KeelFactory }>;
  const mod = await load();
  const instance = await mod.default();
  return new instance.Database();
}

export function readResults(payload: string): QueryResult[] {
  const parsed = JSON.parse(payload) as QueryResult[];
  if (!Array.isArray(parsed)) {
    throw new Error("engine returned a result that was not a list");
  }
  return parsed;
}

export function readSchema(payload: string): SchemaTable[] {
  const parsed = JSON.parse(payload) as SchemaTable[] | { error?: string };
  if (!Array.isArray(parsed)) {
    throw new Error(parsed.error || "schema could not be read");
  }
  return parsed;
}
