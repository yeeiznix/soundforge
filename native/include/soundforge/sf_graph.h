// SoundForge G2 P3/P4 — graph C ABI (§3.3, §4.2)
// 9 sf_graph_* exports: 6 mutators with routing-rule enforcement + audit +
// set_handle_error, and 3 routing queries (validate / topo order / mixer).
#pragma once

#include <stdint.h>

#include "soundforge/sf_project.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SF_NODE_SOURCE    1
#define SF_NODE_PROCESSOR 2
#define SF_NODE_OUTPUT    3
#define SF_NODE_BUS       4

/* out_id: caller buffer of 37+ bytes (36 UUID + NUL); filled with new node UUID */
sf_result_t sf_graph_add_node(sf_project_t* p, int32_t kind, const char* name, char* out_id);
/* Cascades: removes all incident edges. */
sf_result_t sf_graph_remove_node(sf_project_t* p, const char* node_id);

sf_result_t sf_graph_add_edge(sf_project_t* p, const char* from_id, const char* to_id,
                              int32_t from_port, int32_t to_port, char* out_id);
sf_result_t sf_graph_remove_edge(sf_project_t* p, const char* edge_id);

sf_result_t sf_graph_set_mixer(sf_project_t* p, const char* node_id,
                               double gain_db, double pan, int32_t mute, int32_t solo);
/* preset_id: NULL or "" clears the reference */
sf_result_t sf_graph_set_preset(sf_project_t* p, const char* node_id, const char* preset_id);

/* -------------------------------------------------------------------------
 * Routing queries (G2 P4, §4.3)
 * ----------------------------------------------------------------------- */

/* Whole-graph structural report (JSON {status, warnings, errors, stats}).
   Returns SF_OK only when the report is fully clean (status "ok");
   SF_E_SCHEMA for warnings OR errors, matching sf_project_health_check
   (report still valid and filled; inspect report["status"] to distinguish).
   Checks: duplicate node/edge ids, dangling edges, edge-into-source,
   edge-out-of-output, cycles, orphan (unreachable) nodes as warnings. */
sf_result_t sf_graph_validate(sf_project_t* p, char* report_buf, size_t report_cap);

/* Topological order (Kahn; sources first, deterministic). JSON: {"order":[...]}.
   SF_E_SCHEMA when the graph contains a cycle. malloc'd; free with sf_free_string(). */
sf_result_t sf_graph_topological_order(sf_project_t* p, char** out_json, size_t* out_len);

/* Static routing-desk evaluation (§4.3). JSON: {muted, order, outputs, soloed};
   per output {clipped, nodeId, peakGainLin, routes}, per route
   {gainLin, nodeIds, sourceId}. Scalar coefficients only — no audio buffers.
   SF_E_SCHEMA on cycle. malloc'd; free with sf_free_string(). */
sf_result_t sf_graph_evaluate_mixer(sf_project_t* p, char** out_json, size_t* out_len);

#ifdef __cplusplus
}
#endif
