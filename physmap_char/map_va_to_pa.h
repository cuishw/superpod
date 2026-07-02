/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MAP_VA_TO_PA_H
#define MAP_VA_TO_PA_H

#include <stdint.h>

int map_va_to_pa(uint32_t gpu_id, uint64_t va_addr, uint64_t pa_addr,
		 uint64_t size, int is_cpu_mem);
int unmap_va_to_pa(uint32_t gpu_id, uint64_t va_addr, uint64_t size,
		   int is_cpu_mem);

#endif /* MAP_VA_TO_PA_H */
