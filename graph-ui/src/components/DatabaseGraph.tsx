import React, { useEffect, useState, useMemo, useRef, useCallback } from "react";
import { callTool } from "../api/rpc";
import { Button } from "./ui/button";
import {
  ZoomIn,
  ZoomOut,
  Maximize2,
  Minimize2,
  RotateCcw,
  Search,
  Key,
  Link2,
  Table as TableIcon,
  Layers,
  Filter,
  Sparkles,
  RefreshCw,
  X,
  Copy,
  Check,
} from "lucide-react";

interface DatabaseGraphProps {
  project: string;
}

interface TableColumn {
  name: string;
  data_type: string;
  is_pk?: boolean;
  is_fk?: boolean;
  is_nullable?: boolean;
  is_unique?: boolean;
}

interface TableInfo {
  name: string;
  schema?: string;
  table_type?: string;
  row_count_estimate?: number;
  is_lookup?: boolean;
  columns?: TableColumn[];
}

interface TableRelation {
  id: string;
  sourceTable: string;
  targetTable: string;
  sourceColumn: string;
  targetColumn: string;
  cardinality: string;
  label: string;
}

interface DatabaseSchemaResult {
  database_name?: string;
  dialect?: string;
  domain_label?: string;
  schemas?: {
    name: string;
    tables: TableInfo[];
  }[];
}

interface NodePosition {
  x: number;
  y: number;
}

function parseSchemaText(text: string): DatabaseSchemaResult {
  const lines = text.split("\n");
  const tables: TableInfo[] = [];
  let currentTable: TableInfo | null = null;
  let inColumns = false;

  for (const rawLine of lines) {
    const line = rawLine.trim();
    if (!line) continue;

    const tableMatch = line.match(/^([a-zA-Z0-9_]+)\s*\((Table|View)\):$/i);
    if (tableMatch) {
      if (currentTable) {
        tables.push(currentTable);
      }
      currentTable = {
        name: tableMatch[1],
        table_type: tableMatch[2],
        columns: [],
      };
      inColumns = false;
      continue;
    }

    if (currentTable) {
      if (line.startsWith("properties:")) {
        try {
          const jsonStr = line.replace(/^properties:\s*/, "");
          const props = JSON.parse(jsonStr);
          currentTable.row_count_estimate = props.row_count;
          currentTable.is_lookup = props.is_lookup;
          if (props.table_type) currentTable.table_type = props.table_type;
        } catch {}
      } else if (line.startsWith("columns:")) {
        inColumns = true;
      } else if (inColumns && line.startsWith("- ")) {
        const colName = line.replace(/^-\s*/, "").trim();
        if (colName) {
          currentTable.columns?.push({
            name: colName,
            data_type: "TEXT",
            is_pk:
              colName.toLowerCase() === "id" ||
              colName.toLowerCase().endsWith("id") ||
              colName.toLowerCase().startsWith("fldid"),
          });
        }
      }
    }
  }

  if (currentTable) {
    tables.push(currentTable);
  }

  return {
    dialect: "MySQL / Relational",
    schemas: [{ name: "default", tables }],
  };
}

// ── Memoized Table Node Component (GPU Accelerated) ──
const TableNode = React.memo(
  ({
    table,
    position,
    isSelected,
    isHovered,
    isConnected,
    onMouseDown,
    onMouseEnter,
    onMouseLeave,
  }: {
    table: TableInfo;
    position: NodePosition;
    isSelected: boolean;
    isHovered: boolean;
    isConnected: boolean;
    onMouseDown: (e: React.MouseEvent) => void;
    onMouseEnter: () => void;
    onMouseLeave: () => void;
  }) => {
    return (
      <div
        onMouseDown={onMouseDown}
        onMouseEnter={onMouseEnter}
        onMouseLeave={onMouseLeave}
        style={{
          transform: `translate3d(${position.x}px, ${position.y}px, 0)`,
          width: "270px",
          willChange: "transform",
        }}
        className={`absolute top-0 left-0 rounded-xl border select-none cursor-pointer transition-colors ${
          isSelected
            ? "border-cyan-400 bg-[#081e2b] ring-2 ring-cyan-400/80 shadow-[0_0_25px_rgba(6,182,212,0.3)] z-30"
            : isHovered || isConnected
            ? "border-cyan-500/80 bg-[#071822] shadow-[0_0_15px_rgba(6,182,212,0.2)] z-20"
            : "border-cyan-950/80 bg-[#051118] hover:border-cyan-700/60 z-10"
        }`}
      >
        {/* Table Header */}
        <div
          className={`px-3.5 py-2 rounded-t-xl border-b flex items-center justify-between ${
            isSelected
              ? "bg-gradient-to-r from-cyan-900/60 to-blue-900/40 border-cyan-500/40"
              : "bg-black/30 border-cyan-950/60"
          }`}
        >
          <div className="flex items-center gap-2 overflow-hidden">
            <TableIcon
              className={`w-3.5 h-3.5 shrink-0 ${
                isSelected ? "text-cyan-300" : "text-cyan-400/80"
              }`}
            />
            <span
              className={`font-bold font-mono text-xs truncate ${
                isSelected ? "text-cyan-100" : "text-white/90"
              }`}
              title={table.name}
            >
              {table.name}
            </span>
          </div>

          <div className="flex items-center gap-1.5 shrink-0">
            {table.is_lookup && (
              <span className="text-[9px] px-1.5 py-0.2 rounded bg-amber-950/80 border border-amber-600/50 text-amber-300 font-mono">
                lookup
              </span>
            )}
            {table.row_count_estimate !== undefined && (
              <span className="text-[9px] font-mono text-white/40">
                {table.row_count_estimate.toLocaleString()}r
              </span>
            )}
          </div>
        </div>

        {/* Columns List */}
        <div className="p-1 max-h-52 overflow-hidden font-mono text-[11px] divide-y divide-white/[0.04]">
          {table.columns && table.columns.length > 0 ? (
            table.columns.slice(0, 10).map((col) => (
              <div
                key={col.name}
                className={`flex items-center justify-between px-2 py-0.5 rounded ${
                  col.is_pk
                    ? "bg-amber-500/10 text-amber-200 font-semibold"
                    : col.is_fk
                    ? "bg-blue-500/10 text-cyan-200 font-semibold"
                    : "text-white/70"
                }`}
              >
                <div className="flex items-center gap-1.5 truncate">
                  {col.is_pk ? (
                    <Key className="w-2.5 h-2.5 text-amber-400 shrink-0" />
                  ) : col.is_fk ? (
                    <Link2 className="w-2.5 h-2.5 text-cyan-400 shrink-0" />
                  ) : (
                    <span className="w-2.5 h-2.5 shrink-0 text-center text-[9px] text-white/20">
                      •
                    </span>
                  )}
                  <span className="truncate">{col.name}</span>
                </div>

                <span className="text-[10px] text-white/35 shrink-0 ml-2">
                  {col.data_type}
                </span>
              </div>
            ))
          ) : (
            <div className="px-2 py-2 text-center text-white/30 text-[10px]">
              No columns defined
            </div>
          )}

          {table.columns && table.columns.length > 10 && (
            <div className="px-2 py-0.5 text-center text-[10px] text-cyan-400/60">
              +{table.columns.length - 10} more columns
            </div>
          )}
        </div>
      </div>
    );
  }
);

// ── Memoized SVG Relation Link Component ──
const RelationPath = React.memo(
  ({
    rel,
    srcPos,
    tgtPos,
    isHovered,
    onMouseEnter,
    onMouseLeave,
  }: {
    rel: TableRelation;
    srcPos: NodePosition;
    tgtPos: NodePosition;
    isHovered: boolean;
    onMouseEnter: () => void;
    onMouseLeave: () => void;
  }) => {
    const cardW = 270;
    const cardH = 180;

    const srcCenterX = srcPos.x + cardW / 2;
    const srcCenterY = srcPos.y + cardH / 2;
    const tgtCenterX = tgtPos.x + cardW / 2;
    const tgtCenterY = tgtPos.y + cardH / 2;

    let srcX = srcPos.x + cardW;
    let srcY = srcCenterY;
    let tgtX = tgtPos.x;
    let tgtY = tgtCenterY;

    if (srcCenterX > tgtCenterX) {
      srcX = srcPos.x;
      tgtX = tgtPos.x + cardW;
    }

    const dx = Math.min(Math.abs(tgtX - srcX) * 0.5 + 30, 200);
    const pathData = `M ${srcX} ${srcY} C ${srcX + (srcX < tgtX ? dx : -dx)} ${srcY}, ${
      tgtX + (tgtX < srcX ? dx : -dx)
    } ${tgtY}, ${tgtX} ${tgtY}`;

    const midX = (srcX + tgtX) / 2;
    const midY = (srcY + tgtY) / 2;

    return (
      <g className="pointer-events-auto cursor-pointer">
        <path
          d={pathData}
          fill="none"
          stroke="transparent"
          strokeWidth="16"
          onMouseEnter={onMouseEnter}
          onMouseLeave={onMouseLeave}
        />
        <path
          d={pathData}
          fill="none"
          stroke={isHovered ? "#38bdf8" : "rgba(6, 182, 212, 0.4)"}
          strokeWidth={isHovered ? "2.5" : "1.5"}
          strokeDasharray={isHovered ? "none" : "4 2"}
          markerEnd={isHovered ? "url(#rel-arrow-active)" : "url(#rel-arrow)"}
        />
        <g
          transform={`translate(${midX}, ${midY})`}
          onMouseEnter={onMouseEnter}
          onMouseLeave={onMouseLeave}
        >
          <rect
            x={-(rel.label.length * 3.5 + 8)}
            y="-9"
            width={rel.label.length * 7 + 16}
            height="18"
            rx="9"
            fill={isHovered ? "#082f49" : "#04141e"}
            stroke={isHovered ? "#38bdf8" : "rgba(6, 182, 212, 0.5)"}
            strokeWidth="1"
          />
          <text
            y="3.5"
            textAnchor="middle"
            fill={isHovered ? "#7dd3fc" : "rgba(255, 255, 255, 0.75)"}
            fontSize="10"
            fontFamily="monospace"
            fontWeight="600"
          >
            {rel.label}
          </text>
        </g>
      </g>
    );
  }
);

export function DatabaseGraph({ project }: DatabaseGraphProps) {
  const [schemaData, setSchemaData] = useState<DatabaseSchemaResult | null>(null);
  const [erdMarkdown, setErdMarkdown] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [selectedTable, setSelectedTable] = useState<string | null>(null);
  const [tableData, setTableData] = useState<any[] | null>(null);
  const [loadingTableData, setLoadingTableData] = useState(false);
  const [searchTerm, setSearchTerm] = useState("");
  const [activeView, setActiveView] = useState<"workbench" | "mermaid" | "explorer">("workbench");
  const [relationFilter, setRelationFilter] = useState<"all" | "connected_only" | "selected_only">("all");
  const [copiedMermaid, setCopiedMermaid] = useState(false);
  const [isFullscreen, setIsFullscreen] = useState(false);

  // High-performance direct Pan & Zoom references
  const panRef = useRef({ x: 60, y: 60 });
  const zoomRef = useRef(0.85);
  const isPanningRef = useRef(false);
  const dragStartRef = useRef({ x: 0, y: 0 });
  const rafRef = useRef<number | null>(null);

  const [zoomDisplay, setZoomDisplay] = useState(85);
  const [hoveredTable, setHoveredTable] = useState<string | null>(null);
  const [hoveredRelation, setHoveredRelation] = useState<string | null>(null);
  const [customPositions, setCustomPositions] = useState<Record<string, NodePosition>>({});

  const draggingTableRef = useRef<string | null>(null);
  const tableDragOffsetRef = useRef({ x: 0, y: 0 });

  const canvasRef = useRef<HTMLDivElement>(null);
  const viewportRef = useRef<HTMLDivElement>(null);

  const updateTransformDOM = useCallback(() => {
    if (viewportRef.current) {
      viewportRef.current.style.transform = `translate3d(${panRef.current.x}px, ${panRef.current.y}px, 0) scale(${zoomRef.current})`;
    }
  }, []);

  const fetchDatabaseInfo = async () => {
    setLoading(true);
    try {
      const schemaRes = await callTool<any>("get_database_schema", { project }).catch(() => null);
      if (typeof schemaRes === "string") {
        setSchemaData(parseSchemaText(schemaRes));
      } else if (schemaRes && typeof schemaRes === "object") {
        setSchemaData(schemaRes);
      }

      const erdRes = await callTool<any>("generate_db_erd", { project }).catch(() => null);
      if (erdRes) {
        if (typeof erdRes === "string") {
          setErdMarkdown(erdRes);
        } else if (erdRes.mermaid) {
          setErdMarkdown(erdRes.mermaid);
        } else if (erdRes.erd) {
          setErdMarkdown(erdRes.erd);
        }
      }
    } catch (err: any) {
      console.warn("Could not fetch DB schema or ERD:", err);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    if (project) {
      fetchDatabaseInfo();
    }
  }, [project]);

  useEffect(() => {
    updateTransformDOM();
  }, [updateTransformDOM, activeView]);

  // Load sample table data when table is selected
  useEffect(() => {
    if (selectedTable && project) {
      setLoadingTableData(true);
      callTool<any>("get_table_data", { project, table_name: selectedTable, limit: 10 })
        .then((res) => {
          if (Array.isArray(res)) {
            setTableData(res);
          } else if (res?.rows && Array.isArray(res.rows)) {
            setTableData(res.rows);
          } else {
            setTableData(null);
          }
        })
        .catch(() => setTableData(null))
        .finally(() => setLoadingTableData(false));
    } else {
      setTableData(null);
    }
  }, [selectedTable, project]);

  // Flatten all tables
  const allTables: TableInfo[] = useMemo(() => {
    if (!schemaData?.schemas) return [];
    const list: TableInfo[] = [];
    for (const s of schemaData.schemas) {
      if (s.tables) {
        for (const t of s.tables) {
          list.push({ ...t, schema: s.name });
        }
      }
    }
    return list;
  }, [schemaData]);

  // Parse relations from Mermaid & Column Naming conventions
  const { relations, tablesWithRelations } = useMemo(() => {
    const tableMap = new Map<string, TableInfo>();
    allTables.forEach((t) => tableMap.set(t.name.toLowerCase(), t));

    const relList: TableRelation[] = [];
    const addedRelKeys = new Set<string>();

    if (erdMarkdown) {
      const relRegex = /([a-zA-Z0-9_]+)\s*(\|\|--o\{|\}o--\|\||\|\|--\|\||\}o--o\{|--)\s*([a-zA-Z0-9_]+)\s*:\s*"?([^"\n\r]*)"?/g;
      let match;
      while ((match = relRegex.exec(erdMarkdown)) !== null) {
        const src = match[1];
        const card = match[2];
        const tgt = match[3];
        const label = match[4].trim();
        const key = `${src}->${tgt}:${label}`;

        if (!addedRelKeys.has(key) && src !== tgt) {
          addedRelKeys.add(key);
          const colName = label.replace(/^has\s+/i, "").trim() || "id";
          relList.push({
            id: key,
            sourceTable: src,
            targetTable: tgt,
            sourceColumn: colName,
            targetColumn: "id",
            cardinality: card,
            label: label || colName,
          });
        }
      }
    }

    allTables.forEach((srcTable) => {
      srcTable.columns?.forEach((col) => {
        if (col.is_pk) return;
        const fldMatch = col.name.match(/^fld([A-Z0-9_][a-zA-Z0-9_]*?)(?:Id|ID)$/);
        const idMatch = col.name.match(/^([a-zA-Z0-9_]+?)(?:_id|Id|ID)$/i);

        const targetCandidates: string[] = [];
        if (fldMatch && fldMatch[1]) {
          targetCandidates.push(`tbl${fldMatch[1]}`, fldMatch[1], `tbl_${fldMatch[1]}`);
        }
        if (idMatch && idMatch[1]) {
          targetCandidates.push(idMatch[1], `${idMatch[1]}s`, `tbl${idMatch[1]}`);
        }

        for (const cand of targetCandidates) {
          const matchedTarget = tableMap.get(cand.toLowerCase());
          if (matchedTarget && matchedTarget.name !== srcTable.name) {
            const key = `${srcTable.name}->${matchedTarget.name}:${col.name}`;
            if (!addedRelKeys.has(key)) {
              addedRelKeys.add(key);
              relList.push({
                id: key,
                sourceTable: srcTable.name,
                targetTable: matchedTarget.name,
                sourceColumn: col.name,
                targetColumn: matchedTarget.columns?.find((c) => c.is_pk)?.name || "id",
                cardinality: "}o--||",
                label: `has ${col.name}`,
              });
              col.is_fk = true;
            }
            break;
          }
        }
      });
    });

    const relatedSet = new Set<string>();
    relList.forEach((r) => {
      relatedSet.add(r.sourceTable);
      relatedSet.add(r.targetTable);
    });

    return { relations: relList, tablesWithRelations: relatedSet };
  }, [allTables, erdMarkdown]);

  // Compute Layout Positions for all tables
  const defaultPositions = useMemo(() => {
    const pos: Record<string, NodePosition> = {};
    const cardWidth = 270;
    const cardHeight = 240;
    const colCount = Math.max(3, Math.ceil(Math.sqrt(allTables.length * 1.5)));

    allTables.forEach((t, i) => {
      const col = i % colCount;
      const row = Math.floor(i / colCount);
      pos[t.name] = {
        x: col * (cardWidth + 140) + 40,
        y: row * (cardHeight + 80) + 40,
      };
    });
    return pos;
  }, [allTables]);

  const getTablePos = useCallback(
    (name: string): NodePosition => {
      return customPositions[name] || defaultPositions[name] || { x: 0, y: 0 };
    },
    [customPositions, defaultPositions]
  );

  // Filter tables
  const visibleTables = useMemo(() => {
    let list = allTables;

    if (relationFilter === "connected_only") {
      list = list.filter((t) => tablesWithRelations.has(t.name));
    } else if (relationFilter === "selected_only" && selectedTable) {
      const connected = new Set<string>([selectedTable]);
      relations.forEach((r) => {
        if (r.sourceTable === selectedTable) connected.add(r.targetTable);
        if (r.targetTable === selectedTable) connected.add(r.sourceTable);
      });
      list = list.filter((t) => connected.has(t.name));
    }

    if (searchTerm) {
      const q = searchTerm.toLowerCase();
      list = list.filter(
        (t) =>
          t.name.toLowerCase().includes(q) ||
          t.columns?.some((c) => c.name.toLowerCase().includes(q))
      );
    }
    return list;
  }, [allTables, relationFilter, selectedTable, relations, tablesWithRelations, searchTerm]);

  const visibleTableNames = useMemo(
    () => new Set(visibleTables.map((t) => t.name)),
    [visibleTables]
  );

  const visibleRelations = useMemo(() => {
    return relations.filter(
      (r) => visibleTableNames.has(r.sourceTable) && visibleTableNames.has(r.targetTable)
    );
  }, [relations, visibleTableNames]);

  // Smooth 60fps Pan and Zoom Event Handlers
  const handleWheel = (e: React.WheelEvent) => {
    e.preventDefault();
    const zoomFactor = e.deltaY < 0 ? 1.08 : 0.92;
    const newZoom = Math.min(Math.max(zoomRef.current * zoomFactor, 0.15), 2.5);

    if (canvasRef.current) {
      const rect = canvasRef.current.getBoundingClientRect();
      const mouseX = e.clientX - rect.left;
      const mouseY = e.clientY - rect.top;

      panRef.current = {
        x: mouseX - (mouseX - panRef.current.x) * (newZoom / zoomRef.current),
        y: mouseY - (mouseY - panRef.current.y) * (newZoom / zoomRef.current),
      };
      zoomRef.current = newZoom;
      setZoomDisplay(Math.round(newZoom * 100));
      updateTransformDOM();
    }
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (e.target === canvasRef.current || (e.target as HTMLElement).tagName === "svg") {
      isPanningRef.current = true;
      dragStartRef.current = {
        x: e.clientX - panRef.current.x,
        y: e.clientY - panRef.current.y,
      };
    }
  };

  const handleMouseMove = (e: React.MouseEvent) => {
    if (isPanningRef.current) {
      panRef.current = {
        x: e.clientX - dragStartRef.current.x,
        y: e.clientY - dragStartRef.current.y,
      };
      if (rafRef.current) cancelAnimationFrame(rafRef.current);
      rafRef.current = requestAnimationFrame(updateTransformDOM);
    } else if (draggingTableRef.current) {
      const currentTbl = draggingTableRef.current;
      const newX = (e.clientX - panRef.current.x) / zoomRef.current - tableDragOffsetRef.current.x;
      const newY = (e.clientY - panRef.current.y) / zoomRef.current - tableDragOffsetRef.current.y;

      setCustomPositions((prev) => ({
        ...prev,
        [currentTbl]: { x: Math.max(0, newX), y: Math.max(0, newY) },
      }));
    }
  };

  const handleMouseUp = () => {
    isPanningRef.current = false;
    draggingTableRef.current = null;
    if (rafRef.current) cancelAnimationFrame(rafRef.current);
  };

  const handleTableMouseDown = (tableName: string, e: React.MouseEvent) => {
    e.stopPropagation();
    const pos = getTablePos(tableName);
    const clickX = (e.clientX - panRef.current.x) / zoomRef.current;
    const clickY = (e.clientY - panRef.current.y) / zoomRef.current;

    draggingTableRef.current = tableName;
    tableDragOffsetRef.current = {
      x: clickX - pos.x,
      y: clickY - pos.y,
    };
    setSelectedTable(tableName);
  };

  const handleFitToScreen = () => {
    if (visibleTables.length === 0) return;
    let minX = Infinity,
      minY = Infinity,
      maxX = -Infinity,
      maxY = -Infinity;

    visibleTables.forEach((t) => {
      const p = getTablePos(t.name);
      minX = Math.min(minX, p.x);
      minY = Math.min(minY, p.y);
      maxX = Math.max(maxX, p.x + 280);
      maxY = Math.max(maxY, p.y + 240);
    });

    if (canvasRef.current) {
      const { clientWidth, clientHeight } = canvasRef.current;
      const contentW = maxX - minX + 120;
      const contentH = maxY - minY + 120;
      const fitZoom = Math.min(
        Math.max(Math.min(clientWidth / contentW, clientHeight / contentH), 0.2),
        1.2
      );

      zoomRef.current = fitZoom;
      panRef.current = {
        x: (clientWidth - contentW * fitZoom) / 2 - minX * fitZoom + 60,
        y: (clientHeight - contentH * fitZoom) / 2 - minY * fitZoom + 60,
      };
      setZoomDisplay(Math.round(fitZoom * 100));
      updateTransformDOM();
    }
  };

  const handleResetView = () => {
    zoomRef.current = 0.85;
    panRef.current = { x: 60, y: 60 };
    setZoomDisplay(85);
    setCustomPositions({});
    updateTransformDOM();
  };

  const handleCopyMermaid = () => {
    if (erdMarkdown) {
      navigator.clipboard.writeText(erdMarkdown);
      setCopiedMermaid(true);
      setTimeout(() => setCopiedMermaid(false), 2000);
    }
  };

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full bg-[#03080d] text-cyan-400 font-mono text-sm">
        <div className="flex flex-col items-center gap-3">
          <div className="w-8 h-8 border-2 border-cyan-500/30 border-t-cyan-400 rounded-full animate-spin" />
          <span className="tracking-wide">Introspecting relational database entities for {project}...</span>
        </div>
      </div>
    );
  }

  const hasDbContent = allTables.length > 0 || erdMarkdown;

  return (
    <div
      className={`w-full h-full flex flex-col bg-[#03080d] text-white overflow-hidden select-none ${
        isFullscreen ? "fixed inset-0 z-50 bg-[#03080d]" : "relative"
      }`}
    >
      {/* ── Workbench Top Header & Toolbar ── */}
      <div className="h-14 border-b border-cyan-950/80 px-6 flex items-center justify-between bg-[#061118] shrink-0 shadow-md">
        <div className="flex items-center gap-4">
          <div className="flex items-center gap-2.5">
            <div className="w-7 h-7 rounded-lg bg-gradient-to-br from-cyan-500 to-blue-600 flex items-center justify-center text-white shadow-[0_0_12px_rgba(6,182,212,0.4)]">
              <TableIcon className="w-4 h-4" />
            </div>
            <div>
              <div className="flex items-center gap-2">
                <span className="font-bold text-sm tracking-wide text-cyan-100 font-mono">
                  Database ERD Workbench
                </span>
                <span className="text-[10px] font-mono px-2 py-0.5 rounded-full bg-cyan-950 border border-cyan-700/50 text-cyan-300 font-bold">
                  {allTables.length} Tables
                </span>
                <span className="text-[10px] font-mono px-2 py-0.5 rounded-full bg-purple-950 border border-purple-700/50 text-purple-300">
                  {relations.length} Relations
                </span>
              </div>
            </div>
          </div>
        </div>

        {/* Center/Right Toolbar */}
        <div className="flex items-center gap-3">
          {hasDbContent && (
            <>
              {/* Search Bar */}
              <div className="relative">
                <Search className="w-3.5 h-3.5 text-cyan-400/60 absolute left-2.5 top-2.5" />
                <input
                  type="text"
                  placeholder="Search tables & fields..."
                  value={searchTerm}
                  onChange={(e) => setSearchTerm(e.target.value)}
                  className="w-52 h-8 pl-8 pr-7 text-xs bg-black/50 border border-cyan-900/40 rounded-lg focus:outline-none focus:border-cyan-400 text-cyan-100 placeholder-white/30 font-mono transition-all focus:w-64"
                />
                {searchTerm && (
                  <button
                    onClick={() => setSearchTerm("")}
                    className="absolute right-2.5 top-2 text-xs text-white/40 hover:text-white"
                  >
                    <X className="w-3.5 h-3.5" />
                  </button>
                )}
              </div>

              {/* View Switcher Tabs */}
              <div className="flex rounded-lg border border-cyan-900/50 bg-black/40 p-0.5">
                <button
                  onClick={() => setActiveView("workbench")}
                  className={`px-3 py-1 text-xs font-semibold rounded-md transition-all flex items-center gap-1.5 ${
                    activeView === "workbench"
                      ? "bg-gradient-to-r from-cyan-600 to-blue-600 text-white shadow-[0_0_10px_rgba(6,182,212,0.3)]"
                      : "text-white/60 hover:text-white"
                  }`}
                >
                  <Layers className="w-3.5 h-3.5" />
                  Workbench ERD
                </button>
                <button
                  onClick={() => setActiveView("explorer")}
                  className={`px-3 py-1 text-xs font-semibold rounded-md transition-all flex items-center gap-1.5 ${
                    activeView === "explorer"
                      ? "bg-gradient-to-r from-cyan-600 to-blue-600 text-white shadow-[0_0_10px_rgba(6,182,212,0.3)]"
                      : "text-white/60 hover:text-white"
                  }`}
                >
                  <TableIcon className="w-3.5 h-3.5" />
                  Schema Explorer
                </button>
                <button
                  onClick={() => setActiveView("mermaid")}
                  className={`px-3 py-1 text-xs font-semibold rounded-md transition-all flex items-center gap-1.5 ${
                    activeView === "mermaid"
                      ? "bg-gradient-to-r from-cyan-600 to-blue-600 text-white shadow-[0_0_10px_rgba(6,182,212,0.3)]"
                      : "text-white/60 hover:text-white"
                  }`}
                >
                  <Sparkles className="w-3.5 h-3.5" />
                  Mermaid Source
                </button>
              </div>

              {/* Relation Filter dropdown */}
              {activeView === "workbench" && (
                <div className="flex items-center gap-1.5 bg-black/40 border border-cyan-900/50 rounded-lg px-2.5 py-1 text-xs text-white/70">
                  <Filter className="w-3 h-3 text-cyan-400" />
                  <select
                    value={relationFilter}
                    onChange={(e: any) => setRelationFilter(e.target.value)}
                    className="bg-transparent text-xs text-cyan-200 outline-none cursor-pointer font-mono"
                  >
                    <option value="all" className="bg-[#08151c]">All Tables ({allTables.length})</option>
                    <option value="connected_only" className="bg-[#08151c]">With Relations ({tablesWithRelations.size})</option>
                    {selectedTable && (
                      <option value="selected_only" className="bg-[#08151c]">Selected ({selectedTable}) & Links</option>
                    )}
                  </select>
                </div>
              )}
            </>
          )}

          <Button
            variant="outline"
            size="sm"
            onClick={fetchDatabaseInfo}
            className="h-8 border-cyan-900/60 bg-cyan-950/20 hover:bg-cyan-900/40 text-cyan-300 gap-1.5 text-xs font-mono"
          >
            <RefreshCw className="w-3.5 h-3.5" />
            Refresh
          </Button>

          <Button
            variant="outline"
            size="sm"
            onClick={() => setIsFullscreen(!isFullscreen)}
            className="h-8 px-2.5 border-cyan-900/60 bg-cyan-950/20 hover:bg-cyan-900/40 text-cyan-300"
            title={isFullscreen ? "Exit Fullscreen" : "Fullscreen Workbench"}
          >
            {isFullscreen ? <Minimize2 className="w-3.5 h-3.5" /> : <Maximize2 className="w-3.5 h-3.5" />}
          </Button>
        </div>
      </div>

      {/* ── Main View Content Area ── */}
      <div className="flex-1 relative overflow-hidden flex bg-[#03080d]">
        {!hasDbContent ? (
          <div className="flex-1 flex items-center justify-center p-8">
            <div className="max-w-md w-full bg-[#07131a] border border-cyan-900/40 rounded-2xl p-8 text-center shadow-2xl">
              <div className="w-16 h-16 mx-auto mb-4 rounded-2xl bg-cyan-950/60 border border-cyan-500/40 flex items-center justify-center text-cyan-300 text-2xl shadow-[0_0_25px_rgba(6,182,212,0.2)]">
                🗄️
              </div>
              <h3 className="text-lg font-bold text-cyan-100 mb-2">No Database Schema Loaded</h3>
              <p className="text-xs text-white/60 leading-relaxed mb-6">
                Index your relational MySQL or SQLite database to visualize entity relationship
                diagrams, foreign keys, and tables.
              </p>
              <Button onClick={fetchDatabaseInfo} className="w-full bg-cyan-600 hover:bg-cyan-500">
                Introspect Database Now
              </Button>
            </div>
          </div>
        ) : activeView === "workbench" ? (
          /* ── 1. MySQL Workbench Smooth Pan & Zoom Canvas ── */
          <div
            ref={canvasRef}
            onWheel={handleWheel}
            onMouseDown={handleMouseDown}
            onMouseMove={handleMouseMove}
            onMouseUp={handleMouseUp}
            className="flex-1 w-full h-full relative overflow-hidden select-none cursor-grab active:cursor-grabbing"
            style={{
              backgroundImage: `radial-gradient(circle, rgba(6,182,212,0.08) 1px, transparent 1px), radial-gradient(circle, rgba(59,130,246,0.04) 1px, transparent 1px)`,
              backgroundSize: "28px 28px, 140px 140px",
              backgroundColor: "#03080d",
            }}
          >
            {/* Direct GPU Hardware-Accelerated Viewport */}
            <div
              ref={viewportRef}
              className="absolute top-0 left-0 origin-top-left"
              style={{
                willChange: "transform",
              }}
            >
              {/* ── SVG Relations Overlay ── */}
              <svg
                className="absolute top-0 left-0 pointer-events-none"
                style={{
                  width: "10000px",
                  height: "10000px",
                  overflow: "visible",
                }}
              >
                <defs>
                  <marker
                    id="rel-arrow"
                    viewBox="0 0 10 10"
                    refX="8"
                    refY="5"
                    markerWidth="6"
                    markerHeight="6"
                    orient="auto-start-reverse"
                  >
                    <path d="M 0 1 L 10 5 L 0 9 z" fill="#06b6d4" />
                  </marker>
                  <marker
                    id="rel-arrow-active"
                    viewBox="0 0 10 10"
                    refX="8"
                    refY="5"
                    markerWidth="7"
                    markerHeight="7"
                    orient="auto-start-reverse"
                  >
                    <path d="M 0 1 L 10 5 L 0 9 z" fill="#38bdf8" />
                  </marker>
                </defs>

                {visibleRelations.map((rel) => {
                  const srcPos = getTablePos(rel.sourceTable);
                  const tgtPos = getTablePos(rel.targetTable);
                  const isHovered =
                    hoveredRelation === rel.id ||
                    hoveredTable === rel.sourceTable ||
                    hoveredTable === rel.targetTable ||
                    selectedTable === rel.sourceTable ||
                    selectedTable === rel.targetTable;

                  return (
                    <RelationPath
                      key={rel.id}
                      rel={rel}
                      srcPos={srcPos}
                      tgtPos={tgtPos}
                      isHovered={Boolean(isHovered)}
                      onMouseEnter={() => setHoveredRelation(rel.id)}
                      onMouseLeave={() => setHoveredRelation(null)}
                    />
                  );
                })}
              </svg>

              {/* ── Table Nodes on Canvas ── */}
              {visibleTables.map((tbl) => {
                const pos = getTablePos(tbl.name);
                const isSelected = selectedTable === tbl.name;
                const isHovered = hoveredTable === tbl.name;
                const isConnected =
                  Boolean(selectedTable) &&
                  relations.some(
                    (r) =>
                      (r.sourceTable === selectedTable && r.targetTable === tbl.name) ||
                      (r.targetTable === selectedTable && r.sourceTable === tbl.name)
                  );

                return (
                  <TableNode
                    key={tbl.name}
                    table={tbl}
                    position={pos}
                    isSelected={isSelected}
                    isHovered={isHovered}
                    isConnected={isConnected}
                    onMouseDown={(e) => handleTableMouseDown(tbl.name, e)}
                    onMouseEnter={() => setHoveredTable(tbl.name)}
                    onMouseLeave={() => setHoveredTable(null)}
                  />
                );
              })}
            </div>

            {/* ── Floating Canvas Controls (Zoom / Pan / Reset) ── */}
            <div className="absolute bottom-6 left-6 flex items-center gap-1.5 bg-[#07141d]/95 border border-cyan-900/60 rounded-xl p-1.5 shadow-2xl z-40">
              <button
                onClick={() => {
                  zoomRef.current = Math.min(zoomRef.current + 0.15, 2.5);
                  setZoomDisplay(Math.round(zoomRef.current * 100));
                  updateTransformDOM();
                }}
                className="w-8 h-8 rounded-lg bg-black/40 hover:bg-cyan-950 text-cyan-300 flex items-center justify-center transition-colors"
                title="Zoom In"
              >
                <ZoomIn className="w-4 h-4" />
              </button>
              <button
                onClick={() => {
                  zoomRef.current = Math.max(zoomRef.current - 0.15, 0.15);
                  setZoomDisplay(Math.round(zoomRef.current * 100));
                  updateTransformDOM();
                }}
                className="w-8 h-8 rounded-lg bg-black/40 hover:bg-cyan-950 text-cyan-300 flex items-center justify-center transition-colors"
                title="Zoom Out"
              >
                <ZoomOut className="w-4 h-4" />
              </button>
              <div className="w-px h-5 bg-cyan-900/50 mx-1" />
              <button
                onClick={handleFitToScreen}
                className="px-2.5 h-8 rounded-lg bg-black/40 hover:bg-cyan-950 text-cyan-300 font-mono text-xs flex items-center gap-1 transition-colors"
                title="Fit All Tables to Screen"
              >
                <Maximize2 className="w-3.5 h-3.5" />
                Fit Screen
              </button>
              <button
                onClick={handleResetView}
                className="w-8 h-8 rounded-lg bg-black/40 hover:bg-cyan-950 text-cyan-300 flex items-center justify-center transition-colors"
                title="Reset Pan & Zoom"
              >
                <RotateCcw className="w-3.5 h-3.5" />
              </button>
              <span className="text-[11px] font-mono text-cyan-400/70 px-2">
                {zoomDisplay}%
              </span>
            </div>

            {/* ── Selected Table Inspector Drawer ── */}
            {selectedTable && (
              <div className="absolute top-4 right-4 bottom-4 w-96 bg-[#061219]/95 border border-cyan-900/60 rounded-2xl shadow-2xl flex flex-col z-40 overflow-hidden">
                <div className="p-4 border-b border-cyan-950 flex items-center justify-between bg-black/40">
                  <div className="flex items-center gap-2">
                    <TableIcon className="w-4 h-4 text-cyan-400" />
                    <h3 className="font-bold font-mono text-sm text-cyan-100 truncate max-w-[240px]">
                      {selectedTable}
                    </h3>
                  </div>
                  <button
                    onClick={() => setSelectedTable(null)}
                    className="w-6 h-6 rounded-md hover:bg-white/10 flex items-center justify-center text-white/50 hover:text-white"
                  >
                    <X className="w-4 h-4" />
                  </button>
                </div>

                <div className="flex-1 overflow-y-auto p-4 space-y-5">
                  {/* Table Stats */}
                  <div className="grid grid-cols-2 gap-2 text-xs font-mono">
                    <div className="bg-black/30 border border-cyan-950 rounded-lg p-2.5">
                      <span className="text-white/40 block text-[10px]">TOTAL COLUMNS</span>
                      <span className="text-cyan-300 font-bold text-sm">
                        {allTables.find((t) => t.name === selectedTable)?.columns?.length || 0}
                      </span>
                    </div>
                    <div className="bg-black/30 border border-cyan-950 rounded-lg p-2.5">
                      <span className="text-white/40 block text-[10px]">ROW ESTIMATE</span>
                      <span className="text-emerald-400 font-bold text-sm">
                        {allTables.find((t) => t.name === selectedTable)?.row_count_estimate?.toLocaleString() || "—"}
                      </span>
                    </div>
                  </div>

                  {/* Connected Relationships */}
                  <div>
                    <h4 className="text-[11px] font-mono uppercase text-white/40 mb-2 tracking-wider">
                      Connected Relationships
                    </h4>
                    <div className="space-y-1.5 font-mono text-xs">
                      {relations
                        .filter(
                          (r) => r.sourceTable === selectedTable || r.targetTable === selectedTable
                        )
                        .map((r) => (
                          <div
                            key={r.id}
                            className="p-2 rounded-lg bg-cyan-950/30 border border-cyan-900/40 flex items-center justify-between"
                          >
                            <span className="text-cyan-200">
                              {r.sourceTable === selectedTable ? `→ ${r.targetTable}` : `← ${r.sourceTable}`}
                            </span>
                            <span className="text-[10px] text-white/40">{r.label}</span>
                          </div>
                        ))}
                      {relations.filter(
                        (r) => r.sourceTable === selectedTable || r.targetTable === selectedTable
                      ).length === 0 && (
                        <div className="text-xs text-white/30 italic">No direct relationships detected</div>
                      )}
                    </div>
                  </div>

                  {/* Sample records table */}
                  <div>
                    <h4 className="text-[11px] font-mono uppercase text-white/40 mb-2 tracking-wider flex items-center justify-between">
                      <span>Sample Table Records</span>
                      {loadingTableData && <span className="text-cyan-400 animate-pulse">Loading...</span>}
                    </h4>

                    {tableData && tableData.length > 0 ? (
                      <div className="bg-black/40 border border-cyan-950 rounded-lg overflow-x-auto max-h-56">
                        <table className="w-full text-left text-[10px] font-mono">
                          <thead className="bg-white/5 text-white/40 border-b border-white/5">
                            <tr>
                              {Object.keys(tableData[0]).map((k) => (
                                <th key={k} className="p-1.5 whitespace-nowrap">
                                  {k}
                                </th>
                              ))}
                            </tr>
                          </thead>
                          <tbody className="divide-y divide-white/5">
                            {tableData.map((row, idx) => (
                              <tr key={idx} className="hover:bg-white/5">
                                {Object.values(row).map((v: any, cIdx) => (
                                  <td key={cIdx} className="p-1.5 truncate max-w-[120px] text-white/80">
                                    {v === null ? "NULL" : String(v)}
                                  </td>
                                ))}
                              </tr>
                            ))}
                          </tbody>
                        </table>
                      </div>
                    ) : (
                      <div className="p-3 text-center text-xs font-mono text-white/30 border border-dashed border-cyan-950 rounded-lg">
                        {loadingTableData ? "Fetching sample rows..." : "No cached records available"}
                      </div>
                    )}
                  </div>
                </div>
              </div>
            )}
          </div>
        ) : activeView === "mermaid" ? (
          /* ── 2. Mermaid Source & Markdown ── */
          <div className="flex-1 overflow-auto p-8 flex flex-col items-center bg-[#040d12]">
            <div className="w-full max-w-5xl bg-[#08151c] border border-cyan-900/40 rounded-2xl p-6 shadow-2xl">
              <div className="flex justify-between items-center mb-4 pb-3 border-b border-cyan-950">
                <div className="flex items-center gap-2">
                  <span className="text-xs font-mono text-cyan-300 uppercase tracking-wider font-bold">
                    Mermaid ERD Definition
                  </span>
                  <span className="text-[11px] text-white/40 font-mono">
                    ({relations.length} relationships mapped)
                  </span>
                </div>
                <Button
                  size="sm"
                  variant="outline"
                  onClick={handleCopyMermaid}
                  className="h-8 border-cyan-900/60 bg-cyan-950/30 hover:bg-cyan-900/50 text-cyan-300 gap-1.5 text-xs font-mono"
                >
                  {copiedMermaid ? <Check className="w-3.5 h-3.5 text-emerald-400" /> : <Copy className="w-3.5 h-3.5" />}
                  {copiedMermaid ? "Copied!" : "Copy Mermaid"}
                </Button>
              </div>
              <pre className="p-4 bg-black/70 rounded-xl border border-cyan-950 font-mono text-xs text-cyan-200/90 whitespace-pre overflow-x-auto leading-relaxed max-h-[70vh]">
                {erdMarkdown || "No ERD generated yet"}
              </pre>
            </div>
          </div>
        ) : (
          /* ── 3. Schema Explorer Table List & Detail ── */
          <div className="flex-1 flex overflow-hidden">
            <div className="w-80 border-r border-cyan-950/80 bg-[#061118]/80 flex flex-col overflow-y-auto shrink-0">
              <div className="p-3 border-b border-cyan-950 text-[11px] font-mono text-white/40 uppercase tracking-wider flex justify-between items-center">
                <span>Tables ({visibleTables.length})</span>
              </div>
              <div className="p-2 space-y-1">
                {visibleTables.map((tbl) => (
                  <button
                    key={tbl.name}
                    onClick={() => setSelectedTable(tbl.name)}
                    className={`w-full text-left px-3 py-2 rounded-lg text-xs font-mono transition-all flex items-center justify-between ${
                      selectedTable === tbl.name
                        ? "bg-cyan-950 text-cyan-200 border border-cyan-600/50 shadow-[0_0_12px_rgba(6,182,212,0.2)]"
                        : "text-white/70 hover:bg-white/5 hover:text-white"
                    }`}
                  >
                    <span className="truncate">{tbl.name}</span>
                    <span className="text-[10px] text-white/30 ml-2 shrink-0">
                      {tbl.columns?.length || 0} cols
                    </span>
                  </button>
                ))}
              </div>
            </div>

            <div className="flex-1 overflow-y-auto p-6 bg-[#040d12]">
              {selectedTable ? (
                <div className="max-w-4xl mx-auto space-y-6">
                  <div className="flex items-center justify-between border-b border-cyan-950 pb-4">
                    <div>
                      <h2 className="text-xl font-bold font-mono text-cyan-200">{selectedTable}</h2>
                      <p className="text-xs text-white/50 font-mono mt-1">
                        Table entity in project {project}
                      </p>
                    </div>
                  </div>

                  <div className="bg-[#08151c] border border-cyan-900/40 rounded-xl overflow-hidden shadow-xl">
                    <div className="p-3 bg-black/40 border-b border-cyan-950 text-xs font-semibold text-cyan-100">
                      Column Schema
                    </div>
                    <table className="w-full text-left text-xs font-mono">
                      <thead className="bg-white/5 text-white/40 border-b border-white/5">
                        <tr>
                          <th className="p-3">Column Name</th>
                          <th className="p-3">Type</th>
                          <th className="p-3">Attributes</th>
                          <th className="p-3">Nullable</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-white/5">
                        {allTables
                          .find((t) => t.name === selectedTable)
                          ?.columns?.map((col) => (
                            <tr key={col.name} className="hover:bg-white/5">
                              <td className="p-3 font-semibold text-white/90">{col.name}</td>
                              <td className="p-3 text-cyan-300">{col.data_type}</td>
                              <td className="p-3">
                                {col.is_pk && (
                                  <span className="px-1.5 py-0.5 rounded bg-amber-500/20 text-amber-300 mr-1.5 text-[10px]">
                                    PRIMARY KEY
                                  </span>
                                )}
                                {col.is_fk && (
                                  <span className="px-1.5 py-0.5 rounded bg-blue-500/20 text-blue-300 text-[10px]">
                                    FOREIGN KEY
                                  </span>
                                )}
                              </td>
                              <td className="p-3 text-white/50">
                                {col.is_nullable ? "YES" : "NO"}
                              </td>
                            </tr>
                          ))}
                      </tbody>
                    </table>
                  </div>

                  {/* Sample rows */}
                  {tableData && tableData.length > 0 && (
                    <div className="bg-[#08151c] border border-cyan-900/40 rounded-xl overflow-hidden shadow-xl">
                      <div className="p-3 bg-black/40 border-b border-cyan-950 text-xs font-semibold text-cyan-100 flex justify-between">
                        <span>Sample Cached Records (Limit 10)</span>
                        <span className="text-white/40 text-[11px] font-mono">
                          {tableData.length} records
                        </span>
                      </div>
                      <div className="overflow-x-auto">
                        <table className="w-full text-left text-xs font-mono">
                          <thead className="bg-white/5 text-white/40 border-b border-white/5">
                            <tr>
                              {Object.keys(tableData[0]).map((k) => (
                                <th key={k} className="p-2.5 whitespace-nowrap">
                                  {k}
                                </th>
                              ))}
                            </tr>
                          </thead>
                          <tbody className="divide-y divide-white/5">
                            {tableData.map((row, rIdx) => (
                              <tr key={rIdx} className="hover:bg-white/5">
                                {Object.values(row).map((v: any, cIdx) => (
                                  <td key={cIdx} className="p-2.5 truncate max-w-[200px] text-white/80">
                                    {v === null ? "NULL" : String(v)}
                                  </td>
                                ))}
                              </tr>
                            ))}
                          </tbody>
                        </table>
                      </div>
                    </div>
                  )}
                </div>
              ) : (
                <div className="flex items-center justify-center h-full text-white/30 text-xs font-mono">
                  Select a table from the left to inspect columns and records.
                </div>
              )}
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
