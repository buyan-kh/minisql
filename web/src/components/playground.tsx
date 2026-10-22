"use client";

import { useCallback, useEffect, useState } from "react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Textarea } from "@/components/ui/textarea";
import { openSession, readResults, readSchema, type QueryResult, type SchemaTable, type Session } from "@/lib/engine";

const DEMO = `CREATE TABLE customer (
  c_custkey INT PRIMARY KEY,
  c_name VARCHAR(32),
  c_nation INT
);
CREATE TABLE orders (
  o_orderkey INT PRIMARY KEY,
  o_custkey INT,
  o_total INT
);
CREATE INDEX orders_cust ON orders (o_custkey);
INSERT INTO customer VALUES
  (1, 'ada', 7),
  (2, 'bea', 3),
  (3, 'cy', 7),
  (4, 'dee', 12);
INSERT INTO orders VALUES
  (10, 1, 120),
  (11, 1, 40),
  (12, 2, 80),
  (13, 3, 15),
  (14, 1, 64);`;

const SUMMARY = `SELECT c_name, SUM(o_total) AS spent
FROM customer
JOIN orders ON customer.c_custkey = orders.o_custkey
GROUP BY c_name
ORDER BY spent DESC;`;

const SAMPLES: { label: string; sql: string }[] = [
  { label: "Spend by customer", sql: SUMMARY },
  {
    label: "Primary key lookup",
    sql: "SELECT c_name, c_nation FROM customer WHERE c_custkey = 1;",
  },
  {
    label: "Orders for one customer",
    sql: "SELECT o_orderkey, o_total FROM orders WHERE o_custkey = 1 ORDER BY o_orderkey;",
  },
  {
    label: "Explain a wide scan",
    sql: "EXPLAIN SELECT o_orderkey FROM orders WHERE o_total >= 0;",
  },
];

type Phase = "loading" | "ready" | "failed";

export function Playground() {
  const [session, setSession] = useState<Session | null>(null);
  const [phase, setPhase] = useState<Phase>("loading");
  const [bootError, setBootError] = useState("");
  const [sql, setSql] = useState(SUMMARY.trim());
  const [busy, setBusy] = useState(false);
  const [results, setResults] = useState<QueryResult[] | null>(null);
  const [schema, setSchema] = useState<SchemaTable[]>([]);
  const [runError, setRunError] = useState("");

  const refreshSchema = useCallback((db: Session) => {
    setSchema(readSchema(db.schema()));
  }, []);

  useEffect(() => {
    let cancelled = false;
    openSession()
      .then((db) => {
        if (cancelled) {
          db.delete();
          return;
        }
        setSession(db);
        setSchema(readSchema(db.schema()));
        setPhase("ready");
      })
      .catch((err: unknown) => {
        if (!cancelled) {
          setBootError(err instanceof Error ? err.message : "the database failed to start");
          setPhase("failed");
        }
      });
    return () => {
      cancelled = true;
    };
  }, []);

  const run = useCallback(
    async (statement: string) => {
      if (!session) {
        return;
      }
      const text = statement.trim();
      if (!text) {
        setResults([]);
        setRunError("");
        return;
      }
      setBusy(true);
      setRunError("");
      try {
        const next = readResults(session.exec(text));
        setResults(next);
        refreshSchema(session);
      } catch (err: unknown) {
        setRunError(err instanceof Error ? err.message : "could not read the result");
      } finally {
        setBusy(false);
      }
    },
    [refreshSchema, session],
  );

  async function loadDemo() {
    if (!session) {
      return;
    }
    setBusy(true);
    setRunError("");
    try {
      session.reset();
      const loaded = readResults(session.exec(DEMO));
      const summary = readResults(session.exec(SUMMARY));
      setSql(SUMMARY.trim());
      setResults([...loaded, ...summary]);
      refreshSchema(session);
    } catch (err: unknown) {
      setRunError(err instanceof Error ? err.message : "demo load failed");
    } finally {
      setBusy(false);
    }
  }

  function clearDatabase() {
    if (!session) {
      return;
    }
    session.reset();
    setResults(null);
    setRunError("");
    refreshSchema(session);
  }

  function explain() {
    const text = sql.trim();
    if (/^explain\b/i.test(text)) {
      void run(text);
      return;
    }
    void run(`EXPLAIN ${text}`);
  }

  return (
    <div className="min-h-full bg-[radial-gradient(circle_at_top_left,oklch(0.93_0.04_70),transparent_36%),var(--background)]">
      <header className="border-b border-border px-4 py-4 md:px-8">
        <div className="mx-auto flex max-w-6xl flex-col gap-3 md:flex-row md:items-end md:justify-between">
          <div>
            <p className="font-mono text-xs tracking-[0.22em] text-primary uppercase">relational engine</p>
            <h1 className="font-heading text-4xl tracking-tight text-foreground">Keel</h1>
            <p className="max-w-xl text-sm text-muted-foreground">
              Write SQL, run it in this tab, and read the plan the cost model chose. Nothing leaves the browser.
            </p>
          </div>
          <div className="flex flex-wrap gap-2">
            <Button type="button" onClick={() => void loadDemo()} disabled={phase !== "ready" || busy}>
              Load harbor books
            </Button>
            <Button type="button" variant="outline" onClick={clearDatabase} disabled={phase !== "ready" || busy}>
              Empty database
            </Button>
          </div>
        </div>
      </header>

      <main className="mx-auto grid max-w-6xl gap-6 px-4 py-6 md:px-8 lg:grid-cols-[17rem_minmax(0,1fr)]">
        <aside className="space-y-6">
          <section>
            <h2 className="mb-2 text-sm font-medium tracking-wide text-muted-foreground uppercase">Schema</h2>
            {phase === "loading" && <p className="text-sm text-muted-foreground">Opening the database…</p>}
            {phase === "failed" && <p className="text-sm text-destructive">{bootError}</p>}
            {phase === "ready" && schema.length === 0 && (
              <p className="text-sm text-muted-foreground">No tables yet. Load the harbor books or create one.</p>
            )}
            <ul className="space-y-3">
              {schema.map((table) => (
                <li key={table.name} className="rounded-lg border border-border bg-card p-3">
                  <div className="flex items-baseline justify-between gap-2">
                    <span className="font-mono text-sm">{table.name}</span>
                    <span className="text-xs text-muted-foreground">{table.rows} rows</span>
                  </div>
                  <ul className="mt-2 space-y-1">
                    {table.columns.map((column) => (
                      <li key={column.name} className="flex items-center justify-between gap-2 text-xs">
                        <span className="font-mono">{column.name}</span>
                        <span className="text-muted-foreground">
                          {column.type}
                          {column.primary_key ? " pk" : ""}
                        </span>
                      </li>
                    ))}
                  </ul>
                  {table.indexes.length > 0 && (
                    <div className="mt-2 flex flex-wrap gap-1">
                      {table.indexes.map((index) => (
                        <Badge key={index.name} variant="outline">
                          {index.unique ? "unique " : ""}
                          {index.name}
                        </Badge>
                      ))}
                    </div>
                  )}
                </li>
              ))}
            </ul>
          </section>
          <section>
            <h2 className="mb-2 text-sm font-medium tracking-wide text-muted-foreground uppercase">Samples</h2>
            <div className="flex flex-col gap-2">
              {SAMPLES.map((sample) => (
                <Button
                  key={sample.label}
                  type="button"
                  variant="secondary"
                  className="justify-start"
                  disabled={phase !== "ready" || busy}
                  onClick={() => {
                    setSql(sample.sql.trim());
                    void run(sample.sql);
                  }}
                >
                  {sample.label}
                </Button>
              ))}
            </div>
          </section>
        </aside>

        <section className="min-w-0 space-y-4">
          <form
            onSubmit={(event) => {
              event.preventDefault();
              void run(sql);
            }}
          >
            <label htmlFor="sql" className="mb-2 block text-sm font-medium">
              Statement
            </label>
            <Textarea
              id="sql"
              value={sql}
              spellCheck={false}
              onChange={(event) => setSql(event.target.value)}
              onKeyDown={(event) => {
                if ((event.metaKey || event.ctrlKey) && event.key === "Enter") {
                  event.preventDefault();
                  void run(sql);
                }
              }}
              className="min-h-40 font-mono text-sm leading-6"
              placeholder="SELECT * FROM customer;"
              disabled={phase !== "ready" || busy}
            />
            <div className="mt-3 flex flex-wrap items-center gap-2">
              <Button type="submit" disabled={phase !== "ready" || busy}>
                {busy ? "Running…" : "Run"}
              </Button>
              <Button type="button" variant="outline" onClick={explain} disabled={phase !== "ready" || busy}>
                Explain
              </Button>
              <span className="text-xs text-muted-foreground">Ctrl or Cmd + Enter runs the editor.</span>
            </div>
          </form>

          {runError && (
            <p className="rounded-lg border border-destructive/40 bg-destructive/10 px-3 py-2 text-sm text-destructive">
              {runError}
            </p>
          )}

          {results === null && phase === "ready" && !runError && (
            <div className="rounded-xl border border-dashed border-border bg-card/70 px-4 py-8 text-sm text-muted-foreground">
              No result yet. Run the statement in the editor, or load the harbor books to create customers and orders.
            </div>
          )}

          {results !== null && results.length === 0 && (
            <div className="rounded-xl border border-dashed border-border bg-card/70 px-4 py-8 text-sm text-muted-foreground">
              The editor was empty, so nothing ran.
            </div>
          )}

          <div className="space-y-4">
            {results?.map((result, index) => (
              <article key={index} className="overflow-hidden rounded-xl border border-border bg-card">
                <header className="flex flex-wrap items-center justify-between gap-2 border-b border-border px-3 py-2 text-xs text-muted-foreground">
                  <span>{result.ok ? result.message || "ok" : "error"}</span>
                  <span>{result.elapsed_ms.toFixed(2)} ms</span>
                </header>
                {!result.ok && <p className="px-3 py-3 text-sm text-destructive">{result.error}</p>}
                {result.plan && (
                  <pre className="overflow-x-auto border-b border-border bg-muted/50 px-3 py-3 font-mono text-xs leading-5 whitespace-pre">
                    {result.plan}
                  </pre>
                )}
                {result.columns.length > 0 && (
                  <div className="overflow-x-auto">
                    <table className="w-full min-w-max border-collapse text-sm">
                      <thead>
                        <tr className="border-b border-border text-left">
                          {result.columns.map((column, columnIndex) => (
                            <th key={`${column}-${columnIndex}`} className="px-3 py-2 font-medium">
                              {column}
                              {result.types[columnIndex] ? (
                                <span className="ml-2 font-mono text-[10px] text-muted-foreground">
                                  {result.types[columnIndex]}
                                </span>
                              ) : null}
                            </th>
                          ))}
                        </tr>
                      </thead>
                      <tbody>
                        {result.rows.length === 0 ? (
                          <tr>
                            <td className="px-3 py-4 text-muted-foreground" colSpan={result.columns.length}>
                              No rows.
                            </td>
                          </tr>
                        ) : (
                          result.rows.map((row, rowIndex) => (
                            <tr key={rowIndex} className="border-b border-border/70 last:border-0">
                              {row.map((cell, cellIndex) => (
                                <td key={cellIndex} className="px-3 py-2 font-mono text-xs">
                                  {cell}
                                </td>
                              ))}
                            </tr>
                          ))
                        )}
                      </tbody>
                    </table>
                  </div>
                )}
              </article>
            ))}
          </div>
        </section>
      </main>
    </div>
  );
}
