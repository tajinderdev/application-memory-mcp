import React, { useState, useEffect, useRef, useCallback } from "react";
import { ErrorBoundary } from "./ErrorBoundary";
import { Activity, Database, GitMerge, FileCode2, Cpu, User, Settings } from "lucide-react";

type NodeData = {
  id: string;
  label: string;
  desc: string;
  icon: React.ElementType;
  x: number;
  y: number;
  color: string;
  glowColor: string;
};

type EdgeData = {
  id: string;
  source: string;
  target: string;
};

const INITIAL_NODES: NodeData[] = [
  { id: '1', label: 'User Query', desc: 'Agent Request', icon: User, x: 500, y: 80, color: 'bg-cyan-500/20 text-cyan-300 border-cyan-500/50', glowColor: 'rgba(6,182,212,0.4)' },
  { id: '2', label: 'Main Orchestrator', desc: 'src/mcp/orchestrator.c', icon: Cpu, x: 500, y: 220, color: 'bg-indigo-500/20 text-indigo-300 border-indigo-500/50', glowColor: 'rgba(99,102,241,0.4)' },
  { id: '3', label: 'Pattern Matching', desc: 'Tier 1 & 2 • History', icon: Activity, x: 250, y: 380, color: 'bg-amber-500/20 text-amber-300 border-amber-500/50', glowColor: 'rgba(245,158,11,0.4)' },
  { id: '4', label: 'Graph Search', desc: 'Tier 3 • Codebase & DB', icon: Database, x: 750, y: 380, color: 'bg-amber-500/20 text-amber-300 border-amber-500/50', glowColor: 'rgba(245,158,11,0.4)' },
  { id: '5', label: 'Synthesize Context', desc: 'Resolve Clashes', icon: GitMerge, x: 500, y: 540, color: 'bg-emerald-500/20 text-emerald-300 border-emerald-500/50', glowColor: 'rgba(16,185,129,0.4)' },
  { id: '6', label: 'Unified Context', desc: 'Engineering Context', icon: FileCode2, x: 500, y: 680, color: 'bg-cyan-500/20 text-cyan-300 border-cyan-500/50', glowColor: 'rgba(6,182,212,0.4)' },
];

const INITIAL_EDGES: EdgeData[] = [
  { id: 'e1-2', source: '1', target: '2' },
  { id: 'e2-3', source: '2', target: '3' },
  { id: 'e2-4', source: '2', target: '4' },
  { id: 'e3-5', source: '3', target: '5' },
  { id: 'e4-5', source: '4', target: '5' },
  { id: 'e5-6', source: '5', target: '6' },
];

const NodeComponent = React.memo(({ 
  node, 
  onMouseDown,
  isSelected,
  onClick
}: { 
  node: NodeData; 
  onMouseDown: (e: React.MouseEvent, id: string) => void;
  isSelected: boolean;
  onClick: (id: string) => void;
}) => {
  const Icon = node.icon;
  
  return (
    <div
      id={`node-${node.id}`}
      onMouseDown={(e) => onMouseDown(e, node.id)}
      onClick={(e) => {
        e.stopPropagation();
        onClick(node.id);
      }}
      className={`absolute w-64 p-4 rounded-xl border backdrop-blur-md cursor-grab active:cursor-grabbing select-none transition-colors duration-150 ${node.color} ${isSelected ? 'z-50' : 'z-10'}`}
      style={{
        transform: `translate3d(calc(${node.x}px - 50%), calc(${node.y}px - 50%), 0)`,
        boxShadow: isSelected
          ? `0 0 24px ${node.glowColor}, 0 0 4px ${node.glowColor}`
          : `0 4px 20px rgba(0,0,0,0.5), inset 0 0 10px ${node.glowColor}`,
        outline: isSelected ? '2px solid rgba(255,255,255,0.6)' : 'none',
      }}
    >
      <div className="flex items-center gap-3 mb-2">
        <div className="p-2 rounded-lg bg-black/40">
          {React.createElement(Icon as React.ComponentType<{ size?: number }>, { size: 20 })}
        </div>
        <div className="font-semibold text-white tracking-wide">{node.label}</div>
      </div>
      <div className="text-xs opacity-80 font-mono text-white/70">{node.desc}</div>
      
      {/* Animated connection points */}
      <div className="absolute -top-1.5 left-1/2 -translate-x-1/2 w-3 h-3 rounded-full bg-white/20 border border-white/40"></div>
      <div className="absolute -bottom-1.5 left-1/2 -translate-x-1/2 w-3 h-3 rounded-full bg-white/20 border border-white/40"></div>
    </div>
  );
});

export function OrchestratorFlow() {
  const [selectedNode, setSelectedNode] = useState<string | null>(null);
  
  // Refs for high-performance rendering
  const nodesRef = useRef(INITIAL_NODES.map(n => ({ ...n })));
  const canvasRef = useRef<HTMLDivElement>(null);
  const svgRef = useRef<SVGSVGElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  
  // State for interaction
  const isDraggingNode = useRef<string | null>(null);
  const isPanning = useRef(false);
  const lastMousePos = useRef({ x: 0, y: 0 });
  
  // Viewport transformation
  const transform = useRef({ x: 0, y: 0, scale: 1 });

  // Draw loop
  const updateDOM = useCallback(() => {
    // Update nodes
    nodesRef.current.forEach(node => {
      const el = document.getElementById(`node-${node.id}`);
      if (el) {
        el.style.transform = `translate3d(calc(${node.x}px - 50%), calc(${node.y}px - 50%), 0)`;
      }
    });

    // Update SVG Paths (both main + glow)
    if (svgRef.current) {
      INITIAL_EDGES.forEach(edge => {
        const pathEl = document.getElementById(`path-${edge.id}`);
        const glowEl = document.getElementById(`path-${edge.id}-glow`);
        const sourceNode = nodesRef.current.find(n => n.id === edge.source);
        const targetNode = nodesRef.current.find(n => n.id === edge.target);
        
        if (sourceNode && targetNode) {
          // Calculate curve
          const startX = sourceNode.x;
          const startY = sourceNode.y + 36; // Approx bottom of node
          const endX = targetNode.x;
          const endY = targetNode.y - 36; // Approx top of node
          
          const ctrlY1 = startY + Math.abs(endY - startY) * 0.4;
          const ctrlY2 = endY - Math.abs(endY - startY) * 0.4;
          
          const d = `M ${startX} ${startY} C ${startX} ${ctrlY1}, ${endX} ${ctrlY2}, ${endX} ${endY}`;
          if (pathEl) pathEl.setAttribute('d', d);
          if (glowEl) glowEl.setAttribute('d', d);
        }
      });
    }

    // Update Canvas Transform
    if (canvasRef.current) {
      canvasRef.current.style.transform = `translate3d(${transform.current.x}px, ${transform.current.y}px, 0) scale(${transform.current.scale})`;
    }
  }, []);

  // Initial draw
  useEffect(() => {
    // Center the graph on load
    if (containerRef.current) {
      const rect = containerRef.current.getBoundingClientRect();
      transform.current.x = (rect.width / 2) - 500; // Center around x=500
      transform.current.y = 20; // Small top padding
    }
    updateDOM();
  }, [updateDOM]);

  // Event Handlers
  const handleMouseDown = useCallback((e: React.MouseEvent) => {
    if (e.button !== 0) return; // Only left click
    
    isPanning.current = true;
    lastMousePos.current = { x: e.clientX, y: e.clientY };
    
    if (containerRef.current) {
      containerRef.current.style.cursor = 'grabbing';
    }
  }, []);

  const handleNodeMouseDown = useCallback((e: React.MouseEvent, id: string) => {
    e.stopPropagation();
    if (e.button !== 0) return;
    
    isDraggingNode.current = id;
    lastMousePos.current = { x: e.clientX, y: e.clientY };
    setSelectedNode(id);
  }, []);

  const handleMouseMove = useCallback((e: React.MouseEvent) => {
    if (isDraggingNode.current) {
      const dx = (e.clientX - lastMousePos.current.x) / transform.current.scale;
      const dy = (e.clientY - lastMousePos.current.y) / transform.current.scale;
      
      const node = nodesRef.current.find(n => n.id === isDraggingNode.current);
      if (node) {
        node.x += dx;
        node.y += dy;
        updateDOM();
      }
      
      lastMousePos.current = { x: e.clientX, y: e.clientY };
    } else if (isPanning.current) {
      const dx = e.clientX - lastMousePos.current.x;
      const dy = e.clientY - lastMousePos.current.y;
      
      transform.current.x += dx;
      transform.current.y += dy;
      
      updateDOM();
      lastMousePos.current = { x: e.clientX, y: e.clientY };
    }
  }, [updateDOM]);

  const handleMouseUp = useCallback(() => {
    isDraggingNode.current = null;
    isPanning.current = false;
    
    if (containerRef.current) {
      containerRef.current.style.cursor = 'grab';
    }
  }, []);

  const handleWheel = useCallback((e: React.WheelEvent) => {
    e.preventDefault();
    
    const zoomSensitivity = 0.001;
    const delta = -e.deltaY * zoomSensitivity;
    
    const newScale = Math.min(Math.max(0.2, transform.current.scale * (1 + delta)), 3);
    
    // Zoom toward mouse cursor
    if (containerRef.current) {
      const rect = containerRef.current.getBoundingClientRect();
      const mouseX = e.clientX - rect.left;
      const mouseY = e.clientY - rect.top;
      
      const dx = (mouseX - transform.current.x) * (1 - newScale / transform.current.scale);
      const dy = (mouseY - transform.current.y) * (1 - newScale / transform.current.scale);
      
      transform.current.x += dx;
      transform.current.y += dy;
    }
    
    transform.current.scale = newScale;
    updateDOM();
  }, [updateDOM]);

  // Controls
  const handleZoomIn = () => {
    transform.current.scale = Math.min(transform.current.scale * 1.2, 3);
    updateDOM();
  };

  const handleZoomOut = () => {
    transform.current.scale = Math.max(transform.current.scale / 1.2, 0.2);
    updateDOM();
  };
  
  const handleReset = () => {
    if (containerRef.current) {
      const rect = containerRef.current.getBoundingClientRect();
      transform.current.x = (rect.width / 2) - 500;
      transform.current.y = 20;
      transform.current.scale = 1;
      updateDOM();
    }
  };

  // Node sidebar details
  const activeNode = selectedNode ? INITIAL_NODES.find(n => n.id === selectedNode) : null;

  return (
    <div className="w-full h-full flex flex-col bg-[#040d12] overflow-hidden text-slate-200">
      <ErrorBoundary>
        
        <div className="flex-none p-4 border-b border-white/10 flex justify-between items-center bg-black/20 backdrop-blur-md z-10">
          <div className="flex items-center gap-3">
            <Settings className="text-cyan-400" />
            <h2 className="text-xl text-cyan-400 font-mono font-bold tracking-wide">Orchestrator Pipeline</h2>
          </div>
          
          <div className="flex items-center gap-2">
            <button onClick={handleZoomIn} className="p-2 rounded bg-white/5 hover:bg-white/10 transition-colors">+</button>
            <button onClick={handleZoomOut} className="p-2 rounded bg-white/5 hover:bg-white/10 transition-colors">-</button>
            <button onClick={handleReset} className="px-3 py-2 rounded bg-cyan-500/20 text-cyan-300 hover:bg-cyan-500/30 transition-colors text-sm font-mono">Reset View</button>
          </div>
        </div>

        <div className="flex-1 flex relative overflow-hidden">
          
          {/* Main Canvas Area */}
          <div 
            ref={containerRef}
            className="flex-1 relative cursor-grab overflow-hidden"
            onMouseDown={handleMouseDown}
            onMouseMove={handleMouseMove}
            onMouseUp={handleMouseUp}
            onMouseLeave={handleMouseUp}
            onWheel={handleWheel}
            onClick={() => setSelectedNode(null)}
          >
            {/* Background Grid */}
            <div className="absolute inset-0" style={{
              backgroundImage: 'radial-gradient(circle at center, rgba(255,255,255,0.05) 1px, transparent 1px)',
              backgroundSize: '30px 30px',
            }}></div>

            {/* Transform Layer */}
            <div ref={canvasRef} className="absolute inset-0 origin-top-left" style={{ willChange: 'transform' }}>
              
              {/* Edges SVG */}
              <svg ref={svgRef} className="absolute inset-0 w-[2000px] h-[2000px] pointer-events-none overflow-visible">
                <defs>
                  <linearGradient id="edge-gradient" x1="0%" y1="0%" x2="0%" y2="100%">
                    <stop offset="0%" stopColor="rgba(56, 189, 248, 0.2)" />
                    <stop offset="100%" stopColor="rgba(56, 189, 248, 0.8)" />
                  </linearGradient>
                  
                  {/* Glowing filter for lines */}
                  <filter id="glow" x="-20%" y="-20%" width="140%" height="140%">
                    <feGaussianBlur stdDeviation="3" result="blur" />
                    <feMerge>
                      <feMergeNode in="blur" />
                      <feMergeNode in="SourceGraphic" />
                    </feMerge>
                  </filter>
                </defs>
                
                {INITIAL_EDGES.map(edge => (
                  <g key={edge.id}>
                    {/* Base invisible path for easier hover (optional, if we wanted interactive edges) */}
                    <path
                      id={`path-${edge.id}-base`}
                      fill="none"
                      stroke="transparent"
                      strokeWidth={15}
                    />
                    {/* Glow layer */}
                    <path
                      id={`path-${edge.id}-glow`}
                      fill="none"
                      stroke="rgba(6, 182, 212, 0.2)"
                      strokeWidth={4}
                      filter="url(#glow)"
                    />
                    {/* Animated data flow line */}
                    <path
                      id={`path-${edge.id}`}
                      fill="none"
                      stroke="url(#edge-gradient)"
                      strokeWidth={2}
                      strokeLinecap="round"
                      className="animate-[dash_3s_linear_infinite]"
                      style={{
                        strokeDasharray: '10 15',
                      }}
                    />
                  </g>
                ))}
              </svg>

              {/* Nodes Layer */}
              {INITIAL_NODES.map(node => (
                <NodeComponent 
                  key={node.id} 
                  node={node} 
                  onMouseDown={handleNodeMouseDown} 
                  isSelected={selectedNode === node.id}
                  onClick={setSelectedNode}
                />
              ))}

            </div>
          </div>

          {/* Details Sidebar */}
          <div className={`w-80 bg-black/40 border-l border-white/10 backdrop-blur-xl transition-all duration-300 flex flex-col ${selectedNode ? 'translate-x-0' : 'translate-x-full absolute right-0 top-0 bottom-0'}`}>
            {activeNode ? (
              <div className="p-6 h-full flex flex-col">
                <div className={`w-12 h-12 rounded-xl flex items-center justify-center mb-4 ${activeNode.color.replace('/20', '/30')}`}>
                  {React.createElement(activeNode.icon as React.ComponentType<{ size?: number; className?: string }>, { size: 24, className: 'text-white' })}
                </div>
                
                <h3 className="text-2xl font-bold text-white mb-2">{activeNode.label}</h3>
                <p className="text-cyan-400 font-mono text-sm mb-6 pb-4 border-b border-white/10">{activeNode.desc}</p>
                
                <div className="space-y-4 flex-1">
                  <div>
                    <div className="text-xs uppercase text-white/50 tracking-wider mb-1">Status</div>
                    <div className="flex items-center gap-2 text-emerald-400 bg-emerald-400/10 px-3 py-1.5 rounded w-fit">
                      <div className="w-2 h-2 rounded-full bg-emerald-400 animate-pulse"></div>
                      Operational
                    </div>
                  </div>
                  
                  <div>
                    <div className="text-xs uppercase text-white/50 tracking-wider mb-1">Throughput</div>
                    <div className="font-mono text-lg">124 req/s</div>
                  </div>
                  
                  <div>
                    <div className="text-xs uppercase text-white/50 tracking-wider mb-2">Metrics</div>
                    <div className="h-2 w-full bg-white/5 rounded overflow-hidden flex">
                      <div className="h-full bg-cyan-500 w-[60%]"></div>
                      <div className="h-full bg-amber-500 w-[20%]"></div>
                    </div>
                    <div className="flex justify-between text-xs text-white/40 mt-1 font-mono">
                      <span>Hit: 60%</span>
                      <span>Miss: 20%</span>
                    </div>
                  </div>
                </div>
                
                <button className="w-full py-3 rounded-lg bg-white/5 hover:bg-white/10 transition-colors font-mono text-sm border border-white/10">
                  View Logs
                </button>
              </div>
            ) : null}
          </div>

        </div>

        <style>{`
          @keyframes dash {
            from { stroke-dashoffset: 50; }
            to { stroke-dashoffset: 0; }
          }
        `}</style>
      </ErrorBoundary>
    </div>
  );
}
