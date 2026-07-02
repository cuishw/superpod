/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MAP_VA_TO_PA_H
#define MAP_VA_TO_PA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t gpu_index_to_id(uint32_t gpu_index);
int map_va_to_pa(uint32_t gpu_id, uint64_t va_addr, uint64_t pa_addr,
		 uint64_t size, int is_cpu_mem);
int unmap_va_to_pa(uint32_t gpu_id, uint64_t va_addr, uint64_t size,
		   int is_cpu_mem);

#ifdef __cplusplus
}
#endif

#endif /* MAP_VA_TO_PA_H */
