import { useEffect, useState, useMemo } from "react";
import { ErrorBoundary } from "./ErrorBoundary";
import { Button } from "./ui/button";

interface ContextEvent {
  id: number;
  timestamp_ms: number;
  event_type: string;
  payload: any;
}

interface ContextGraphProps {
  project: string;
}

export function ContextGraph({ project }: ContextGraphProps) {
  const [events, setEvents] = useState<ContextEvent[]>([]);
  const [loading, setLoading] = useState(true);
  const [filterType, setFilterType] = useState<string>("ALL");
  const [searchTerm, setSearchTerm] = useState("");
  const [expandedIds, setExpandedIds] = useState<Set<number>>(new Set());

  const fetchEvents = () => {
    setLoading(true);
    fetch(`/api/layout-context?project=${encodeURIComponent(project)}`)
      .then((r) => r.json())
      .then((data: ContextEvent[]) => {
        setEvents(Array.isArray(data) ? data : []);
      })
      .catch((e) => {
        console.error("Failed to load context events:", e);
        setEvents([]);
      })
      .finally(() => setLoading(false));
  };

  useEffect(() => {
    if (project) {
      fetchEvents();
    }
  }, [project]);

  const toggleExpand = (id: number) => {
    setExpandedIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };

  const filteredEvents = useMemo(() => {
    return events.filter((evt) => {
      if (filterType !== "ALL" && evt.event_type !== filterType) return false;
      if (searchTerm) {
        const q = searchTerm.toLowerCase();
        const str = JSON.stringify(evt).toLowerCase();
        if (!str.includes(q)) return false;
      }
      return true;
    });
  }, [events, filterType, searchTerm]);

  const eventTypes = useMemo(() => {
    const types = new Set(events.map((e) => e.event_type));
    return Array.from(types);
  }, [events]);

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full bg-[#040d12] text-cyan-400 font-mono text-sm">
        <div className="flex flex-col items-center gap-3">
          <div className="w-8 h-8 border-2 border-cyan-500/30 border-t-cyan-400 rounded-full animate-spin" />
          <span>Retrieving Historical Context for {project}...</span>
        </div>
      </div>
    );
  }

  return (
    <div className="w-full h-full flex flex-col bg-[#040d12] text-white select-none overflow-hidden">
      {/* Header & Filter Controls */}
      <div className="h-12 border-b border-border/30 px-6 flex items-center justify-between bg-[#08151c]/90 backdrop-blur-md shrink-0">
        <div className="flex items-center gap-3">
          <span className="w-2.5 h-2.5 rounded-full bg-cyan-400 shadow-[0_0_8px_#06b6d4]" />
          <span className="font-semibold text-sm tracking-wide text-cyan-100">
            Historical Context & Engineering Memory
          </span>
          <span className="text-[11px] font-mono px-2 py-0.5 rounded-full bg-cyan-950/60 border border-cyan-700/40 text-cyan-300">
            {events.length} Recorded Events
          </span>
        </div>

        <div className="flex items-center gap-3">
          <input
            type="text"
            placeholder="Search context timeline..."
            value={searchTerm}
            onChange={(e) => setSearchTerm(e.target.value)}
            className="w-52 h-8 px-3 text-xs bg-black/40 border border-border/40 rounded-md focus:outline-none focus:border-cyan-500 text-cyan-100 placeholder-white/30"
          />

          <div className="flex rounded-md border border-border/40 overflow-hidden text-xs">
            <button
              onClick={() => setFilterType("ALL")}
              className={`px-3 py-1 font-medium transition-colors ${
                filterType === "ALL"
                  ? "bg-cyan-600/80 text-white"
                  : "bg-black/20 text-white/60 hover:text-white"
              }`}
            >
              All ({events.length})
            </button>
            {eventTypes.map((t) => (
              <button
                key={t}
                onClick={() => setFilterType(t)}
                className={`px-2.5 py-1 font-medium transition-colors ${
                  filterType === t
                    ? "bg-cyan-600/80 text-white"
                    : "bg-black/20 text-white/60 hover:text-white"
                }`}
              >
                {t.replace("_", " ")}
              </button>
            ))}
          </div>

          <Button variant="outline" size="sm" onClick={fetchEvents}>
            Refresh
          </Button>
        </div>
      </div>

      {/* Main Timeline View */}
      <div className="flex-1 overflow-y-auto p-8 bg-[#040d12]">
        <ErrorBoundary>
          {filteredEvents.length === 0 ? (
            <div className="flex flex-col items-center justify-center h-full max-w-md mx-auto text-center">
              <div className="w-16 h-16 rounded-2xl bg-cyan-950/40 border border-cyan-500/20 flex items-center justify-center text-cyan-400 text-2xl mb-4 shadow-[0_0_20px_rgba(6,182,212,0.1)]">
                🕰️
              </div>
              <h3 className="text-base font-semibold text-cyan-100 mb-2">
                No Context Events Found
              </h3>
              <p className="text-xs text-white/50 leading-relaxed mb-6">
                Session events, code change correlations, failure traces, and engineering patterns
                will automatically populate here as agentic workflows execute.
              </p>
              <Button size="sm" onClick={fetchEvents} className="bg-cyan-600 hover:bg-cyan-500">
                Check Again
              </Button>
            </div>
          ) : (
            <div className="max-w-3xl mx-auto flex flex-col items-center relative pb-16">
              {/* Vertical line through timeline */}
              <div className="absolute top-4 bottom-4 left-1/2 -ml-px w-0.5 bg-gradient-to-b from-cyan-500/50 via-purple-500/30 to-emerald-500/20" />

              {filteredEvents.map((evt, idx) => {
                const isExpanded = expandedIds.has(evt.id);
                let title = evt.event_type;
                let summary = "";
                let badgeColor = "border-cyan-500/40 bg-cyan-950/60 text-cyan-300";
                let dotColor = "bg-cyan-400 shadow-[0_0_10px_#06b6d4]";

                if (evt.event_type === "ENGINEERING_PATTERN") {
                  title = evt.payload?.pattern || "Engineering Pattern";
                  summary = evt.payload?.description || "";
                  badgeColor = "border-amber-500/40 bg-amber-950/60 text-amber-300";
                  dotColor = "bg-amber-400 shadow-[0_0_10px_#f59e0b]";
                } else if (evt.event_type === "CHANGE_CORRELATION") {
                  title = "Code Change Correlation";
                  summary = evt.payload?.impact_summary || `${evt.payload?.affected_nodes?.length || 0} symbols affected`;
                  badgeColor = "border-blue-500/40 bg-blue-950/60 text-blue-300";
                  dotColor = "bg-blue-400 shadow-[0_0_10px_#3b82f6]";
                } else if (evt.event_type === "FAILURE_CORRELATION") {
                  title = "Failure Correlation Analysis";
                  summary = evt.payload?.error_message || "Execution failure logged";
                  badgeColor = "border-rose-500/40 bg-rose-950/60 text-rose-300";
                  dotColor = "bg-rose-400 shadow-[0_0_10px_#f43f5e]";
                } else if (evt.event_type === "CORRECTION_MEMORY") {
                  title = "Correction Memory Record";
                  summary = evt.payload?.rationale || "User or self-correction captured";
                  badgeColor = "border-emerald-500/40 bg-emerald-950/60 text-emerald-300";
                  dotColor = "bg-emerald-400 shadow-[0_0_10px_#10b981]";
                }

                return (
                  <div key={evt.id} className="w-full relative flex items-center my-4 group">
                    {/* Center Node Dot */}
                    <div className="absolute left-1/2 -ml-2.5 w-5 h-5 rounded-full bg-[#081720] border-2 border-border/80 flex items-center justify-center z-10">
                      <div className={`w-2 h-2 rounded-full ${dotColor}`} />
                    </div>

                    {/* Timeline Card */}
                    <div
                      className={`w-[calc(50%-28px)] ${
                        idx % 2 === 0 ? "mr-auto text-right" : "ml-auto text-left"
                      }`}
                    >
                      <div
                        onClick={() => toggleExpand(evt.id)}
                        className={`p-4 rounded-xl border bg-[#081720]/90 backdrop-blur-md shadow-xl transition-all cursor-pointer hover:border-cyan-500/60 hover:shadow-[0_0_20px_rgba(6,182,212,0.15)] ${
                          isExpanded ? "border-cyan-400 bg-[#0c2331]" : "border-cyan-900/30"
                        }`}
                      >
                        <div
                          className={`flex items-center gap-2 mb-2 ${
                            idx % 2 === 0 ? "justify-end" : "justify-start"
                          }`}
                        >
                          <span className={`text-[10px] font-mono px-2 py-0.5 rounded-full border ${badgeColor}`}>
                            {evt.event_type}
                          </span>
                          <span className="text-[11px] font-mono text-white/40">
                            {new Date(evt.timestamp_ms).toLocaleTimeString()}
                          </span>
                        </div>

                        <h4 className="font-bold text-sm text-cyan-100 mb-1">{title}</h4>
                        {summary && <p className="text-xs text-white/60 line-clamp-2">{summary}</p>}

                        {/* Expandable JSON Payload */}
                        {isExpanded && evt.payload && (
                          <div className="mt-4 pt-3 border-t border-border/30 text-left">
                            <div className="text-[10px] font-mono uppercase text-white/40 mb-1.5">
                              Event Payload Data:
                            </div>
                            <pre className="p-3 bg-black/60 rounded-lg border border-cyan-950/80 font-mono text-[11px] text-cyan-200/90 whitespace-pre overflow-x-auto max-h-60">
                              {JSON.stringify(evt.payload, null, 2)}
                            </pre>
                          </div>
                        )}

                        <div className="mt-2 text-[10px] font-mono text-cyan-400/60 flex items-center gap-1 justify-end">
                          <span>{isExpanded ? "▲ Collapse Details" : "▼ Expand Details"}</span>
                        </div>
                      </div>
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </ErrorBoundary>
      </div>
    </div>
  );
}
