// SoundForge G2 P3 — graph mutators C ABI (§3.3, §4.2)
// 6 sf_graph_* mutators with full routing-rule enforcement, audit entries, and set_handle_error.
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

#ifdef __cplusplus
}
#endif
