/*
Copyright (C) 2018 Andre Leiradella

This file is part of RALibretro.

RALibretro is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RALibretro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Memory.h"

#include "Application.h"

#include <RA_Interface.h>
#include <rcheevos.h>

#include <time.h>

#define TAG "[MEM] "

#define MAX_MEMORY_REGIONS 64
static uint8_t* g_memoryRegionData[MAX_MEMORY_REGIONS];
static size_t g_memoryRegionSize[MAX_MEMORY_REGIONS];
static unsigned g_memoryRegionCount = 0;
static size_t g_memoryTotalSize = 0;

static unsigned char memoryRead(unsigned addr)
{
  unsigned i;
  for (i = 0; i < g_memoryRegionCount; ++i)
  {
    const size_t size = g_memoryRegionSize[i];
    if (addr < size)
    {
      if (g_memoryRegionData[i] == NULL)
        break;

      return g_memoryRegionData[i][addr];
    }

    addr -= size;
  }

  return 0;
}

static void memoryWrite(unsigned addr, unsigned value)
{
  unsigned i;
  for (i = 0; i < g_memoryRegionCount; ++i)
  {
    const size_t size = g_memoryRegionSize[i];
    if (addr < size)
    {
      if (g_memoryRegionData[i])
        g_memoryRegionData[i][addr] = value;

      break;
    }

    addr -= size;
  }
}

static clock_t g_lastMemoryRefresh = 0;
static unsigned char deferredMemoryRead(unsigned addr)
{
  const clock_t now = clock();
  const clock_t elapsed = now - g_lastMemoryRefresh;
  if (elapsed < CLOCKS_PER_SEC / 100) /* 10ms */
    return '\0';

  g_lastMemoryRefresh = now;

  extern Application app;
  app.refreshMemoryMap();
  return memoryRead(addr);
}

static const char* getMemoryType(int type)
{
  switch (type)
  {
    case RC_MEMORY_TYPE_SAVE_RAM: return "SRAM";
    case RC_MEMORY_TYPE_VIDEO_RAM: return "VRAM";
    case RC_MEMORY_TYPE_UNUSED: return "UNUSED";
    default: return "SYSTEM RAM";
  }
}

void Memory::registerMemoryRegion(int type, uint8_t* data, size_t size, const char* description)
{
  if (size == 0)
    return;

  if (g_memoryRegionCount == MAX_MEMORY_REGIONS)
  {
    _logger->warn(TAG "Too many memory regions to register");
    return;
  }

  if (!data && g_memoryRegionCount > 0 && !g_memoryRegionData[g_memoryRegionCount - 1])
  {
    /* extend null region */
    g_memoryRegionSize[g_memoryRegionCount - 1] += size;
  }
  else if (data && g_memoryRegionCount > 0 &&
    data == (g_memoryRegionData[g_memoryRegionCount - 1] + g_memoryRegionSize[g_memoryRegionCount - 1]))
  {
    /* extend non-null region */
    g_memoryRegionSize[g_memoryRegionCount - 1] += size;
  }
  else
  {
    /* create new region */
    g_memoryRegionData[g_memoryRegionCount] = data;
    g_memoryRegionSize[g_memoryRegionCount] = size;
    ++g_memoryRegionCount;
  }

  g_memoryTotalSize += size;

  _logger->info(TAG "Registered 0x%04X bytes of %s at $%06X (%s)", size, getMemoryType(type), g_memoryTotalSize - size, description);
}

bool Memory::init(libretro::LoggerComponent* logger)
{
  _logger = logger;
  return true;
}

void Memory::destroy()
{
  g_memoryRegionCount = 0;
  g_memoryTotalSize = 0;
  g_lastMemoryRefresh = 0;
  RA_ClearMemoryBanks();
}

void Memory::attachToCore(libretro::Core* core, int consoleId)
{
  /* capture the registered regions */
  uint8_t* memoryRegionData[MAX_MEMORY_REGIONS];
  size_t memoryRegionSize[MAX_MEMORY_REGIONS];
  size_t memoryRegionCount = g_memoryRegionCount;
  size_t memoryTotalSize = g_memoryTotalSize;

  memcpy(memoryRegionData, g_memoryRegionData, sizeof(memoryRegionData));
  memcpy(memoryRegionSize, g_memoryRegionSize, sizeof(memoryRegionSize));

  /* reset and register new regions */
  g_memoryRegionCount = 0;
  g_memoryTotalSize = 0;

  const rc_memory_regions_t* regions = rc_console_memory_regions(consoleId);
  if (regions == NULL || regions->num_regions == 0)
  {
    initializeWithoutRegions(core);
  }
  else
  {
    const retro_memory_map* mmap = core->getMemoryMap();
    if (mmap && mmap->num_descriptors > 0)
      initializeFromMemoryMap(regions, mmap);
    else
      initializeFromUnmappedMemory(regions, core);
  }

  /* if no change is detected, do nothing */
  if (g_memoryTotalSize == memoryTotalSize && g_memoryRegionCount == memoryRegionCount)
  {
    if (memcmp(memoryRegionData, g_memoryRegionData, memoryRegionCount * sizeof(memoryRegionData[0])) == 0 &&
        memcmp(memoryRegionSize, g_memoryRegionSize, memoryRegionCount * sizeof(memoryRegionSize[0])) == 0)
    {
      return;
    }
  }

  /* change detected - update the installed memory banks */
  bool hasValidRegion = false;
  for (size_t i = 0; i < g_memoryRegionCount; i++)
  {
    if (g_memoryRegionData[i] != NULL)
    {
      hasValidRegion = true;
      break;
    }
  }

  if (hasValidRegion)
  {
    RA_ClearMemoryBanks();
    RA_InstallMemoryBank(0, memoryRead, memoryWrite, g_memoryTotalSize);
  }
  else if (g_lastMemoryRefresh == 0)
  {
    g_lastMemoryRefresh = clock();
    RA_ClearMemoryBanks();
    RA_InstallMemoryBank(0, deferredMemoryRead, memoryWrite, g_memoryTotalSize);
  }
}

void Memory::initializeWithoutRegions(libretro::Core* core)
{
  /* no regions specified, assume system RAM followed by save RAM */
  char description[64];
  uint8_t* regionStart = (uint8_t*)core->getMemoryData(RETRO_MEMORY_SYSTEM_RAM);
  size_t regionSize = core->getMemorySize(RETRO_MEMORY_SYSTEM_RAM);
  sprintf(description, "offset 0x%06x", 0);
  registerMemoryRegion(RC_MEMORY_TYPE_SYSTEM_RAM, regionStart, regionSize, description);

  regionStart = (uint8_t*)core->getMemoryData(RETRO_MEMORY_SAVE_RAM);
  regionSize = core->getMemorySize(RETRO_MEMORY_SAVE_RAM);
  registerMemoryRegion(RC_MEMORY_TYPE_SAVE_RAM, regionStart, regionSize, description);
}

static const struct retro_memory_descriptor* getDescriptor(const struct retro_memory_map* mmap, unsigned realAddress)
{
  if (mmap->num_descriptors == 0)
    return NULL;

  const retro_memory_descriptor* desc = mmap->descriptors;
  const retro_memory_descriptor* end = desc + mmap->num_descriptors;

  for (; desc < end; desc++)
  {
    if (desc->select == 0)
    {
      /* if select is 0, attempt to explcitly match the address */
      if (realAddress >= desc->start && realAddress < desc->start + desc->len)
        return desc;
    }
    else
    {
      /* otherwise, attempt to match the address by matching the select bits */
      if (((desc->start ^ realAddress) & desc->select) == 0)
      {
        /* sanity check - make sure the descriptor is large enough to hold the target address */
        if (realAddress - desc->start < desc->len)
          return desc;
      }
    }
  }

  return NULL;
}

static void dumpDescriptors(const retro_memory_map* mmap, libretro::LoggerComponent* logger)
{
  const retro_memory_descriptor* desc = mmap->descriptors;
  const retro_memory_descriptor* end = desc + mmap->num_descriptors;
  int i = 0;

  for (; desc < end; desc++, i++)
  {
    /* log these at info level show they show up in the about dialog, but the function is only
     * called if debug level is enabled. */
    logger->info(TAG "desc[%d]: $%06x (%04x): %s%s", i + 1, desc->start, desc->len,
      desc->addrspace ? desc->addrspace : "", desc->ptr ? "" : "(null)");
  }
}

void Memory::initializeFromMemoryMap(const rc_memory_regions_t* regions, const retro_memory_map* mmap)
{
  char description[64];
  unsigned i;

  if (_logger->logLevel(RETRO_LOG_DEBUG))
    dumpDescriptors(mmap, _logger);

  for (i = 0; i < regions->num_regions; ++i)
  {
    const rc_memory_region_t* region = &regions->region[i];
    size_t regionSize = region->end_address - region->start_address + 1;
    unsigned realAddress = region->real_address;

    while (regionSize > 0)
    {
      const struct retro_memory_descriptor* desc = getDescriptor(mmap, realAddress);
      if (!desc || !desc->ptr)
      {
        if (region->type != RC_MEMORY_TYPE_UNUSED)
          _logger->info(TAG "Could not map region starting at $%06X", realAddress - region->real_address + region->start_address);

        registerMemoryRegion(region->type, NULL, regionSize, "null filler");
        break;
      }

      uint8_t* descStart = (uint8_t*)desc->ptr + desc->offset;
      const size_t offset = realAddress - desc->start;
      uint8_t* regionStart = descStart + offset;
      const size_t descSize = desc->len - offset;
      sprintf(description, "descriptor %u, offset 0x%06X", (int)(desc - mmap->descriptors) + 1, (int)offset);

      if (regionSize > descSize)
      {
        if (descSize == 0)
        {
          if (region->type != RC_MEMORY_TYPE_UNUSED)
            _logger->info(TAG "Could not map region starting at $%06X", realAddress - region->real_address + region->start_address);

          registerMemoryRegion(region->type, NULL, regionSize, "null filler");
          regionSize = 0;
        }
        else
        {
          registerMemoryRegion(region->type, regionStart, descSize, description);
          regionSize -= descSize;
          realAddress += descSize;
        }
      }
      else
      {
        registerMemoryRegion(region->type, regionStart, regionSize, description);
        regionSize = 0;
      }
    }
  }
}

static unsigned rcMemoryTypeToRetroMemoryType(int regionType)
{
  switch (regionType)
  {
  case RC_MEMORY_TYPE_SAVE_RAM:
    return RETRO_MEMORY_SAVE_RAM;
  case RC_MEMORY_TYPE_VIDEO_RAM:
    return RETRO_MEMORY_VIDEO_RAM;
  default:
    return RETRO_MEMORY_SYSTEM_RAM;
  }
}

void Memory::initializeFromUnmappedMemory(const rc_memory_regions_t* regions, libretro::Core* core)
{
  char description[64];
  unsigned i;

  for (i = 0; i < regions->num_regions; ++i)
  {
    const rc_memory_region_t* region = &regions->region[i];
    uint8_t* regionStart;
    const size_t regionSize = region->end_address - region->start_address + 1;
    uint8_t* descStart;
    size_t descSize;
    unsigned base_address = 0;
    unsigned j;

    unsigned retroMemoryType = rcMemoryTypeToRetroMemoryType(region->type);
    for (j = 0; j <= i; ++j)
    {
      const rc_memory_region_t* region2 = &regions->region[j];
      if (rcMemoryTypeToRetroMemoryType(region2->type) == retroMemoryType)
      {
        base_address = region2->start_address;
        break;
      }
    }

    descStart = (uint8_t*)core->getMemoryData(retroMemoryType);
    descSize = core->getMemorySize(retroMemoryType);

    const size_t offset = region->start_address - base_address;
    if (descStart != NULL && offset < descSize)
    {
      regionStart = descStart + offset;
      descSize -= offset;
      sprintf(description, "offset 0x%06X", (int)offset);
    }
    else
    {
      if (region->type != RC_MEMORY_TYPE_UNUSED)
        _logger->info(TAG "Could not map region starting at $%06X", region->start_address);

      regionStart = NULL;
      descSize = 0;
    }

    if (regionSize > descSize)
    {
      if (descSize > 0)
        registerMemoryRegion(region->type, regionStart, descSize, description);

      registerMemoryRegion(region->type, NULL, regionSize - descSize, "null filler");
    }
    else
    {
      registerMemoryRegion(region->type, regionStart, regionSize, description);
    }
  }
}
