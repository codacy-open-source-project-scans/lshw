/*
 * device-tree.cc
 *
 * This module parses the OpenFirmware device tree (published under /proc
 * by the kernel).
 *
 *
 *
 *
 */

#include <algorithm>
#include <errno.h>
#include "version.h"
#include "device-tree.h"
#include "osutils.h"
#include "jedec.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <utility>
#include <map>

__ID("@(#) $Id$");

#define DIMMINFOSIZE 0x200
typedef uint8_t dimminfo_buf[DIMMINFOSIZE];

struct dimminfo
{
  uint8_t version3;
  char serial[16];
  uint16_t version1, version2;
};

#define DEVICETREE "/proc/device-tree"
#define DEVICETREEVPD  "/proc/device-tree/vpd/"

/*
 * Integer properties in device tree are usually represented as a single 
 * "u32 cell" which is an unsigned 32-bit big-endian integer.
 */
static uint32_t get_u32(const string & path)
{
  uint32_t result = 0;
  int fd = open(path.c_str(), O_RDONLY);

  if (fd >= 0)
  {
    if(read(fd, &result, sizeof(result)) != sizeof(result))
      result = 0;

    close(fd);
  }

  return ntohl(result);
}

static uint64_t read_int(FILE *f, size_t length = 4)
{
  vector < uint8_t > bytes(length);
  uint64_t result = 0;
  if (fread(bytes.data(), length, 1, f) != 1)
    return 0;
  for (size_t i = 0; i < length; i ++)
  {
    result += ((uint64_t) bytes[length - i - 1]) << (i * 8);
  }
  return result;
}

/*
 * The reg property is an array of (address, size) pairs. The length of the 
 * address and size are determined by the #address-cells and #size-cells 
 * properties on the parent node, measured in number of "cells" which are 
 * 32 bits wide. The address and size are in big-endian byte order.
 */
struct reg_entry {
  uint64_t address;
  uint64_t size;
};
static vector < reg_entry > get_reg_property(const string & node)
{
  vector < reg_entry > result;

  uint32_t num_address_cells = 1;
  uint32_t num_size_cells = 1;

  if (exists(node + "/../#address-cells"))
    num_address_cells = get_u32(node + "/../#address-cells");
  if (exists(node + "/../#size-cells"))
    num_size_cells = get_u32(node + "/../#size-cells");

  if (num_address_cells > 2 || num_size_cells > 2)
    return result;

  FILE *f = fopen((node + "/reg").c_str(), "r");
  if (f == NULL)
    return result;

  while (true)
  {
    reg_entry entry = {0};
    entry.address = read_int(f, num_address_cells * 4);
    if (feof(f))
      break;
    entry.size = read_int(f, num_size_cells * 4);
    if (feof(f))
      break;
    result.push_back(entry);
  }

  fclose(f);
  return result;
}

static vector < string > get_strings(const string & path,
unsigned int offset = 0)
{
  vector < string > result;
  char *strings = NULL;
  char *curstring = NULL;

  int fd = open(path.c_str(), O_RDONLY);

  if (fd >= 0)
  {
    struct stat buf;

    if (fstat(fd, &buf) == 0)
    {
      strings = (char *) malloc(buf.st_size + 1);
      if (strings)
      {
        memset(strings, 0, buf.st_size + 1);
        if(read(fd, strings, buf.st_size) == buf.st_size)
        {
          curstring = strings + offset;

          while (strlen(curstring))
          {
            result.push_back(string(curstring));
            curstring += strlen(curstring) + 1;
          }
        }

        free(strings);
      }
    }

    close(fd);
  }

  return result;
}


static void scan_devtree_root(hwNode & core)
{
  core.setClock(get_u32(DEVICETREE "/clock-frequency"));
}


static void scan_devtree_bootrom(hwNode & core)
{
  if (exists(DEVICETREE "/rom/boot-rom"))
  {
    hwNode bootrom("firmware",
      hw::memory);
    string upgrade = "";

    bootrom.setProduct(get_string(DEVICETREE "/rom/boot-rom/model"));
    bootrom.setDescription("BootROM");
    bootrom.
      setVersion(get_string(DEVICETREE "/rom/boot-rom/BootROM-version"));

    if ((upgrade =
      get_string(DEVICETREE "/rom/boot-rom/write-characteristic")) != "")
    {
      bootrom.addCapability("upgrade");
      bootrom.addCapability(upgrade);
    }

    vector < reg_entry > regs = get_reg_property(DEVICETREE "/rom/boot-rom");
    if (!regs.empty())
    {
      bootrom.setPhysId(regs[0].address);
      bootrom.setSize(regs[0].size);
    }

    bootrom.claim();
//bootrom.setLogicalName(DEVICETREE "/rom");
    core.addChild(bootrom);
  }

  if (exists(DEVICETREE "/openprom"))
  {
    hwNode openprom("firmware",
      hw::memory);

    if (exists(DEVICETREE "/openprom/ibm,vendor-model"))
	    openprom.setProduct(get_string(DEVICETREE "/openprom/ibm,vendor-model"));
    else
	    openprom.setProduct(get_string(DEVICETREE "/openprom/model"));

    if (exists(DEVICETREE "/openprom/supports-bootinfo"))
      openprom.addCapability("bootinfo");

//openprom.setLogicalName(DEVICETREE "/openprom");
    openprom.setLogicalName(DEVICETREE);
    openprom.claim();
    core.addChild(openprom);
  }
}

static hwNode *add_base_opal_node(hwNode & core)
{
  vector < string >:: iterator it;
  vector < string > compat;
  string basepath = DEVICETREE "/ibm,opal";
  hwNode opal("firmware");

  if (!exists(basepath))
    return NULL;

  pushd(basepath);

  opal.setProduct("OPAL firmware");
  opal.setDescription("skiboot");

  compat = get_strings(basepath + "/compatible");
  for (it = compat.begin(); it != compat.end(); ++it) {
    if (matches(*it, "^ibm,opal-v"))
      opal.addCapability((*it).erase(0,4));
  }

  if (exists(basepath + "/ipmi/compatible") &&
    matches(get_string(basepath + "/ipmi/compatible"), "^ibm,opal-ipmi"))
    opal.addCapability("ipmi");

  if (exists(basepath + "/diagnostics/compatible") &&
    matches(get_string(basepath + "/diagnostics/compatible"), "^ibm,opal-prd"))
    opal.addCapability("prd");

  popd();

  opal.claim();
  return core.addChild(opal);
}

static void scan_devtree_firmware_powernv(hwNode & core)
{
  int n;
  struct dirent **namelist;

  hwNode *opal = add_base_opal_node(core);

  if (!exists(DEVICETREE "/ibm,firmware-versions"))
    return;

  pushd(DEVICETREE "/ibm,firmware-versions");
  n = scandir(".", &namelist, selectfile, alphasort);
  popd();

  if (n <= 0)
    return;

  for (int i = 0; i < n; i++)
  {
    string sname = string(namelist[i]->d_name);
    string fullpath = string(DEVICETREE) + "/ibm,firmware-versions/" + sname;

    if (sname != "linux,phandle" && sname != "name" && sname != "phandle")
    {
      hwNode fwnode("firmware");
      fwnode.setDescription(sname);
      fwnode.setVersion(hw::strip(get_string(fullpath)));
      fwnode.claim();
      if (opal && sname == "skiboot") {
        opal->merge(fwnode);
        free(namelist[i]);
        continue;
      }
      core.addChild(fwnode);
    }
    free(namelist[i]);
  }

  free(namelist);
}

static string cpubusinfo(int cpu)
{
  char buffer[20];

  snprintf(buffer, sizeof(buffer), "cpu@%d", cpu);

  return string(buffer);
}


static void set_cpu(hwNode & cpu, int currentcpu, const string & basepath)
{
  cpu.setProduct(get_string(basepath + "/name"));
  cpu.claim();
  cpu.setBusInfo(cpubusinfo(currentcpu));

  cpu.setSize(get_u32(basepath + "/clock-frequency"));
  cpu.setClock(get_u32(basepath + "/bus-frequency"));

  if (exists(basepath + "/altivec"))
    cpu.addCapability("altivec");

  if (exists(basepath + "/performance-monitor"))
    cpu.addCapability("performance-monitor");
}


static void fill_cache_info(string cache_type, string cachebase,
			    hwNode & cache, hwNode & icache)
{
  cache.claim();
  cache.setDescription(cache_type);
  cache.setSize(get_u32(cachebase + "/d-cache-size"));

  if (exists(cachebase + "/cache-unified"))
    cache.setDescription(cache.getDescription() + " (unified)");
  else
  {
    icache = cache;
    cache.setDescription(cache.getDescription() + " (data)");
    icache.setDescription(icache.getDescription() + " (instruction)");
    icache.setSize(get_u32(cachebase + "/i-cache-size"));
  }
}


static void scan_devtree_cpu(hwNode & core)
{
  struct dirent **namelist;
  int n;
  int currentcpu=0;

  pushd(DEVICETREE "/cpus");
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();
  if (n < 0)
    return;
  else
  {
    for (int i = 0; i < n; i++)
    {
      string basepath =
        string(DEVICETREE "/cpus/") + string(namelist[i]->d_name);
      unsigned long version = 0;
      hwNode cpu("cpu",
        hw::processor);
      struct dirent **cachelist;
      int ncache;

      if (exists(basepath + "/device_type") &&
        hw::strip(get_string(basepath + "/device_type")) != "cpu")
        break;                                    // oops, not a CPU!

      cpu.setDescription("CPU");
      set_cpu(cpu, currentcpu++, basepath);

      version = get_u32(basepath + "/cpu-version");
      if (version != 0)
      {
        int minor = version & 0x00ff;
        int major = (version & 0xff00) >> 8;
        char buffer[20];

        snprintf(buffer, sizeof(buffer), "%lx.%d.%d",
          (version & 0xffff0000) >> 16, major, minor);
        cpu.setVersion(buffer);
      }

      if (hw::strip(get_string(basepath + "/state")) != "running")
        cpu.disable();

      if (exists(basepath + "/d-cache-size"))
      {
        hwNode cache("cache",
          hw::memory);

        cache.claim();
        cache.setDescription("L1 Cache");
        cache.setSize(get_u32(basepath + "/d-cache-size"));
        if (cache.getSize() > 0)
          cpu.addChild(cache);
      }

      pushd(basepath);
      ncache = scandir(".", &cachelist, selectdir, alphasort);
      popd();
      if (ncache > 0)
      {
        for (int j = 0; j < ncache; j++)
        {
          hwNode cache("cache",
            hw::memory);
          hwNode icache("cache",
            hw::memory);
          string cachebase = basepath + "/" + cachelist[j]->d_name;

          if (hw::strip(get_string(cachebase + "/device_type")) != "cache" &&
            hw::strip(get_string(cachebase + "/device_type")) != "l2-cache")
            break;                                // oops, not a cache!

	  cache.setClock(get_u32(cachebase + "/clock-frequency"));
	  fill_cache_info("L2 Cache", cachebase, cache, icache);

          if (icache.getSize() > 0)
            cpu.addChild(icache);

          if (cache.getSize() > 0)
            cpu.addChild(cache);

          free(cachelist[j]);
        }
        free(cachelist);
      }

      core.addChild(cpu);

      free(namelist[i]);
    }
    free(namelist);
  }
}


struct chip_vpd_data
{
  string product;
  string serial;
  string slot;
  string vendor;
};


static void add_chip_vpd(string path, string name,
			 map <uint32_t, chip_vpd_data *> & vpd)
{
  int n;
  struct dirent **dirlist;

  pushd(path + name);

  if (name.substr(0, 9) == "processor" && exists("ibm,chip-id"))
  {
    uint32_t chip_id = get_u32("ibm,chip-id");
    chip_vpd_data *data = new chip_vpd_data();

    if (data)
    {
      if (exists("serial-number"))
        data->serial = hw::strip(get_string("serial-number"));

      if (exists("ibm,loc-code"))
	data->slot = hw::strip(get_string("ibm,loc-code"));

      if (exists("part-number"))
        data->product = hw::strip(get_string("part-number"));

      if (exists("vendor"))
        data->vendor = hw::strip(get_string("vendor"));

      if (exists("fru-number"))
        data->product += " FRU# " + hw::strip(get_string("fru-number"));

      vpd.insert(std::pair<uint32_t, chip_vpd_data *>(chip_id, data));
    }
  }

  n = scandir(".", &dirlist, selectdir, alphasort);
  popd();

  if (n <= 0)
    return;

  for (int i = 0; i < n; i++)
  {
    add_chip_vpd(path + name + "/", dirlist[i]->d_name, vpd);
    free(dirlist[i]);
  }

  free(dirlist);
}


static void scan_chip_vpd(map <uint32_t, chip_vpd_data *> & vpd)
{
  int n;
  struct dirent **namelist;

  if (!exists(DEVICETREEVPD))
    return;

  pushd(DEVICETREEVPD);
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();

  if (n <= 0)
    return;

  for (int i = 0; i < n; i++)
  {
    add_chip_vpd(DEVICETREEVPD, namelist[i]->d_name, vpd);
    free(namelist[i]);
  }

  free(namelist);
}


static void fill_core_vpd(hwNode & cpu, string & basepath,
			  map <uint32_t, chip_vpd_data *> & chip_vpd,
			  map <uint32_t, string> & xscoms)
{
  uint32_t chip_id;
  chip_vpd_data *data;
  string xscom_path;

  if (!exists(basepath + "/ibm,chip-id"))
    return;

  chip_id = get_u32(basepath + "/ibm,chip-id");
  cpu.setConfig("chip-id", chip_id);

  data = chip_vpd[chip_id];
  xscom_path = xscoms[chip_id];

  if (data)
  {
    cpu.setProduct(data->product);
    cpu.setSerial(data->serial);
    cpu.setSlot(data->slot);
    cpu.setVendor(data->vendor);
  }

  if (xscom_path != "")
  {
    vector <string> board_pieces;

    splitlines(hw::strip(get_string(xscom_path + "/board-info")),
	       board_pieces, ' ');
    if (board_pieces.size() > 0)
      cpu.setVendor(board_pieces[0]);

    if (exists(xscom_path + "/serial-number"))
      cpu.setSerial(hw::strip(get_string(xscom_path + "/serial-number")));

    if (exists(xscom_path + "/ibm,slot-location-code"))
      cpu.setSlot(hw::strip(get_string(xscom_path + "/ibm,slot-location-code")));

    if (exists(xscom_path + "/part-number"))
      cpu.setProduct(hw::strip(get_string(xscom_path + "/part-number")));
  }
}

static void set_cpu_config_threads(hwNode & cpu, const string & basepath)
{
  static int threads_per_cpu = 0;

  /* In power systems, there are equal no. of threads per cpu-core */
  if (threads_per_cpu == 0)
  {
    int rc;
    struct stat sbuf;
    string p = hw::strip(basepath + string("/ibm,ppc-interrupt-server#s"));

    /*
     * This file contains as many 32 bit interrupt server numbers, as the
     * number of threads per CPU (in hexadecimal format). st_size gives size
     * in bytes of a file. Hence, grouping by 4 bytes, we get the thread
     * count.
     */
    rc = stat(p.c_str(), &sbuf);
    if (!rc)
      threads_per_cpu = sbuf.st_size / 4;
  }

  cpu.setConfig("threads", threads_per_cpu);
}


static void scan_xscom_node(map <uint32_t, string> & xscoms)
{
  int n;
  struct dirent **namelist;

  pushd(DEVICETREE);
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();

  if (n <= 0)
    return;

  for (int i = 0; i < n; i++) {
    string sname = string(namelist[i]->d_name);
    string fullpath = "";
    int chip_id = 0;

    if (sname.substr(0,5) == "xscom") {
      fullpath = string(DEVICETREE) + "/" + sname;
      chip_id = get_u32(fullpath + "/ibm,chip-id");
      xscoms.insert(std::pair<uint32_t, string>(chip_id, fullpath));
    }

    free(namelist[i]);
  }
  free(namelist);
}

static void scan_devtree_cpu_power(hwNode & core)
{
  int n;
  int currentcpu = 0;
  struct dirent **namelist;
  map <uint32_t, pair<uint32_t, vector <hwNode> > > l2_caches;
  map <uint32_t, vector <hwNode> > l3_caches;
  map <uint32_t, chip_vpd_data *> chip_vpd;
  map <uint32_t, string> xscoms;

  pushd(DEVICETREE "/cpus");
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();
  if (n < 0)
    return;

  /*
   * 'cpus' node contains CPU, L2 and L3 cache nodes. L1 cache information is
   * available under CPU node itself. l2-cache (or next-level-cache) property
   * contains next level cache node phandle/ibm,phanle number.
   * First pass creates cache nodes and second pass will link cache nodes to
   * corresponding CPU nodes.
   */
  for (int i = 0; i < n; i++)
  {
    string product;
    string basepath = string(DEVICETREE "/cpus/") + string(namelist[i]->d_name);
    hwNode cache("cache", hw::memory);
    hwNode icache("cache", hw::memory);
    vector <hwNode> value;

    if (!exists(basepath + "/device_type"))
      continue;

    if (hw::strip(get_string(basepath + "/device_type")) != "cache")
      continue;

    product = hw::strip(get_string(basepath + "/name"));

    if (hw::strip(get_string(basepath + "/status")) != "okay")
    {
      cache.disable();
      icache.disable();
    }

    if (product == "l2-cache")
      fill_cache_info("L2 Cache", basepath, cache, icache);
    else
      fill_cache_info("L3 Cache", basepath, cache, icache);

    if (icache.getSize() > 0)
      value.insert(value.begin(), icache);

    if (cache.getSize() > 0)
      value.insert(value.begin(), cache);

    if (value.size() > 0)
    {
      uint32_t phandle = 0;

      if (exists(basepath + "/phandle"))
        phandle = get_u32(basepath + "/phandle");
      else if (exists(basepath + "/ibm,phandle")) // on pSeries LPARs
        phandle = get_u32(basepath + "/ibm,phandle");

      if (!phandle)
        continue;

      if (product == "l2-cache")
      {
        uint32_t l3_key = 0; // 0 indicating no next level of cache

        if (exists(basepath + "/l2-cache"))
          l3_key = get_u32(basepath + "/l2-cache");
        else if (exists(basepath + "/next-level-cache")) //on OpenPOWER systems
          l3_key = get_u32(basepath + "/next-level-cache");

        pair <uint32_t, vector <hwNode> > p (l3_key, value);
        l2_caches[phandle] = p;
      }
      else if (product == "l3-cache")
      {
        l3_caches[phandle] = value;
      }
    }
  } // first pass end

  /*
   * We have chip level VPD information (like part number, slot, etc).
   * and this information is same for all cores under chip.
   * Fetch chip-level VPD from /vpd node.
   */
  scan_chip_vpd(chip_vpd);

  // List all xscom nodes under DT
  scan_xscom_node(xscoms);

  for (int i = 0; i < n; i++) //second and final pass
  {
    uint32_t l2_key = 0;
    uint32_t version = 0;
    uint32_t reg;
    string basepath = string(DEVICETREE "/cpus/") + string(namelist[i]->d_name);
    hwNode cpu("cpu", hw::processor);

    if (!exists(basepath + "/device_type"))
    {
      free(namelist[i]);
      continue;
    }

    if (hw::strip(get_string(basepath + "/device_type")) != "cpu")
    {
      free(namelist[i]);
      continue;
    }

    cpu.setDescription("CPU");
    cpu.addHint("logo", string("powerpc"));
    set_cpu(cpu, currentcpu++, basepath);

    reg = get_u32(basepath + "/reg");
    cpu.setPhysId(tostring(reg));

    version = get_u32(basepath + "/cpu-version");
    if (version != 0)
      cpu.setVersion(tostring(version));

    fill_core_vpd(cpu, basepath, chip_vpd, xscoms);

    if (hw::strip(get_string(basepath + "/status")) != "okay")
      cpu.disable();

    set_cpu_config_threads(cpu, basepath);

    if (exists(basepath + "/d-cache-size"))
    {
      hwNode cache("cache", hw::memory);
      hwNode icache("cache", hw::memory);

      fill_cache_info("L1 Cache", basepath, cache, icache);

      if (icache.getSize() > 0)
        cpu.addChild(icache);

      if (cache.getSize() > 0)
        cpu.addChild(cache);

      if (hw::strip(get_string(basepath + "/status")) != "okay")
      {
        cache.disable();
        icache.disable();
      }
    }

    if (exists(basepath + "/l2-cache"))
        l2_key = get_u32(basepath + "/l2-cache");
    else if (exists(basepath + "/next-level-cache"))
        l2_key = get_u32(basepath + "/next-level-cache");

    if (l2_key != 0)
    {
      map <uint32_t, pair <uint32_t, vector <hwNode> > >::
        const_iterator got = l2_caches.find(l2_key);

      if (!(got == l2_caches.end()))
        for (uint32_t j = 0; j < (got->second).second.size(); j++)
          cpu.addChild((got->second).second[j]);

      if ((got->second).first != 0) // we have another level of cache
      {
        map <uint32_t, vector <hwNode> >::const_iterator got_l3 =
          l3_caches.find ((got->second).first);

        if (!(got_l3 == l3_caches.end()))
          for (uint32_t j = 0; j < (got_l3->second).size(); j++)
            cpu.addChild((got_l3->second)[j]);
      }
    }

    core.addChild(cpu);

    free(namelist[i]);
  }
  free(namelist);

  map <uint32_t, chip_vpd_data *>::iterator it;
  for (it = chip_vpd.begin(); it != chip_vpd.end(); it++)
    delete it->second;
}

static bool add_memory_bank_mba_dimm(string path,
				     unsigned long serial, hwNode & bank)
{
  bool found = false;
  int n;
  struct dirent **namelist;

  pushd(path);
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();

  if (n < 0)
    return found;

  for (int i = 0; i < n; i++)
  {
    string sname = string(namelist[i]->d_name);
    string fullpath = path + "/" + sname;

    if (found)
    {
      free(namelist[i]);
      continue;
    }

    if (sname.substr(0, 13) == "memory-buffer")
    {
      if (exists(fullpath + "/frequency-mhz"))
      {
        int hz = get_u32(fullpath + "/frequency-mhz") * 1000000;
        bank.setClock(hz);
      }
      found = add_memory_bank_mba_dimm(fullpath, serial, bank);
    } else if (sname.substr(0, 3) == "mba") {
      found = add_memory_bank_mba_dimm(fullpath, serial, bank);
    } else if ((sname.substr(0, 4) == "dimm") &&
	       (get_u32(fullpath + "/serial-number") == serial)) {
      vector < reg_entry > regs = get_reg_property(fullpath);
      bank.setSize(regs[0].size);

      bank.setSlot(hw::strip(get_string(fullpath + "/ibm,slot-location-code")));
      found = true;
    }
    free(namelist[i]);
  }

  free(namelist);
  return found;
}


static void add_memory_bank_spd(string path, hwNode & bank)
{
  char dimmversion[20];
  uint16_t mfg_loc_offset;
  uint16_t rev_offset1;
  uint16_t rev_offset2;
  uint16_t year_offset;
  uint16_t week_offset;
  uint16_t partno_offset;
  uint16_t ver_offset;
  uint16_t serial_offset;
  uint16_t bus_width_offset;
  int fd;
  size_t len = 0;
  dimminfo_buf dimminfo;

  fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return;

  if (read(fd, &dimminfo, 0x80) != 0x80)
  {
    close(fd);
    return;
  }

  /* Read entire SPD eeprom */
  if (dimminfo[2] >= 9) /* DDR3 & DDR4 */
  {
    uint8_t val = (dimminfo[0] >> 4) & 0x7;
    if (val == 1)
      len = 256;
    else if (val == 2)
      len = 512;
  } else if (dimminfo[0] < 15) { /* DDR 2 */
    len = 1 << dimminfo[1];
  }

  if (len > 0x80)
    read(fd, &dimminfo[0x80], len - 0x80);

  close(fd);

  if (dimminfo[2] >= 9) {
    int rank_offset;
    double ns;
    char vendor[5];
    const char *type, *mod_type;

    rev_offset1 = 0x92;
    rev_offset2 = 0x93;
    ver_offset = 0x01;

    if (dimminfo[0x2] >= 0xc) {
      type = "DDR4";
      mfg_loc_offset = 0x142;
      year_offset = 0x143;
      week_offset = 0x144;
      partno_offset = 0x149;
      bus_width_offset = 0x0d;
      serial_offset = 0x145;
      rank_offset = 0xc;

      /*
       * There is no other valid values for the medium- and fine- timebase
       * other than (125ps, 1ps), so we hard-code those here. The fine
       * t_{ckavg}_{min} value is signed. Divide by 2 to get from raw clock
       * to expected data rate
       */
      ns = (((float)dimminfo[0x12] * 0.125) +
	    (((signed char) dimminfo[0x7d]) * 0.001)) / 2;
      snprintf(vendor, sizeof(vendor), "%x%x", dimminfo[0x141], dimminfo[0x140]);
    } else {
      type = "DDR3";
      mfg_loc_offset = 0x77;
      year_offset = 0x78;
      week_offset = 0x79;
      partno_offset = 0x80;
      serial_offset = 0x7a;
      bus_width_offset = 0x08;
      rank_offset = 0x7;

      ns = (dimminfo[0xc] / 2) * (dimminfo[0xa] / (float) dimminfo[0xb]);
      snprintf(vendor, sizeof(vendor), "%x%x", dimminfo[0x76], dimminfo[0x75]);
    }

    /* DDR3 & DDR4 error detection and correction scheme */
    switch ((dimminfo[bus_width_offset] >> 3) & 0x3)
    {
      case 0x00:
        bank.setConfig("errordetection", "none");
        break;
      case 0x01:
        bank.addCapability("ecc");
        bank.setConfig("errordetection", "ecc");
        break;
    }

    // Add DIMM rank
    bank.setConfig("rank", ((dimminfo[rank_offset] >> 3) & 0x7) + 1);

    bank.setClock(1000000000 / ns);
    bank.setVendor(jedec_resolve(vendor));

    char description[100];
    switch(dimminfo[0x3])
    {
      case 0x1:
        mod_type = "RDIMM";
        break;
      case 0x2:
        mod_type = "UDIMM";
        break;
      case 0x3:
        mod_type = "SODIMM";
        break;
      case 0x4:
        mod_type = "LRDIMM";
        break;
      default:
        mod_type = "DIMM";
    }

    snprintf(description, sizeof(description), "%s %s %d MHz (%0.1fns)",
	     mod_type, type, (int) (1000 / ns), ns);
    bank.setDescription(description);
  } else {
    mfg_loc_offset = 0x48;
    rev_offset1 = 0x5b;
    rev_offset2 = 0x5c;
    year_offset = 0x5d;
    week_offset = 0x5e;
    partno_offset = 0x49;
    ver_offset = 0x3e;
    serial_offset = 0x5f;

    switch (dimminfo[0xb] & 0x3) // DDR2 error detection and correction scheme
    {
      case 0x00:
        bank.setConfig("errordetection", "none");
        break;
      case 0x01:
        bank.addCapability("parity");
        bank.setConfig("errordetection", "parity");
        break;
      case 0x02:
      case 0x03:
        bank.addCapability("ecc");
        bank.setConfig("errordetection", "ecc");
        break;
    }
  }

  snprintf(dimmversion, sizeof(dimmversion),
    "%02X%02X,%02X %02X,%02X", dimminfo[rev_offset1],
    dimminfo[rev_offset2], dimminfo[year_offset], dimminfo[week_offset],
    dimminfo[mfg_loc_offset]);
  bank.setProduct(string((char *) &dimminfo[partno_offset], 18));
  bank.setVersion(dimmversion);

  unsigned long serial = be_long(&dimminfo[serial_offset]);
  int version = dimminfo[ver_offset];
  char buff[32];

  snprintf(buff, sizeof(buff), "0x%lx", serial);
  bank.setSerial(buff);

  add_memory_bank_mba_dimm(DEVICETREE, serial, bank);

  snprintf(buff, sizeof(buff), "spd-%d.%d", (version & 0xF0) >> 4, version & 0x0F);
  bank.addCapability(buff);
}

static void add_memory_bank(string name, string path, hwNode & core)
{
  struct dirent **dirlist;
  string product;
  int n;

  hwNode *memory = core.getChild("memory");
  if(!memory)
    memory = core.addChild(hwNode("memory", hw::memory));

  pushd(path + "/" + name);
  if(name.substr(0, 7) == "ms-dimm" ||
     name.substr(0, 18) == "IBM,memory-module@")
  {
    hwNode bank("bank", hw::memory);
    bank.claim(true);
    bank.addHint("icon", string("memory"));

    if(exists("serial-number"))
      bank.setSerial(hw::strip(get_string("serial-number")));

    product = hw::strip(get_string("part-number"));
    if(exists("fru-number"))
    {
      product += " FRU# " + hw::strip(get_string("fru-number"));
    }
    if(product != "")
      bank.setProduct(hw::strip(product));

    string description = "DIMM";
    string package = hw::strip(get_string("ibm,mem-package"));
    if (!package.empty())
      description = package;
    string memtype = hw::strip(get_string("ibm,mem-type"));
    if (!memtype.empty())
      description += " " + memtype;
    if(exists("description"))
      description = hw::strip(get_string("description"));
    bank.setDescription(description);
    if (exists("ibm,chip-id"))
      bank.setConfig("chip-id", get_u32("ibm,chip-id"));

    if(exists("ibm,loc-code"))
      bank.setSlot(hw::strip(get_string("ibm,loc-code")));
    unsigned long size = get_number("size") * 1024 * 1024;
    if (exists("ibm,size"))
      size = get_u32("ibm,size");
    if (size > 0)
      bank.setSize(size);

    // Parse Memory SPD data
    if (exists("spd"))
      add_memory_bank_spd(path + "/" + name + "/spd", bank);

    // Parse Memory SPD data
    if (exists("frequency"))
      bank.setClock(get_u32("frequency"));

    memory->addChild(bank);
  } else if(name.substr(0, 4) == "dimm") {
    hwNode bank("bank", hw::memory);
    bank.claim(true);
    bank.addHint("icon", string("memory"));

    // Parse Memory SPD data
    add_memory_bank_spd(path + name + "/spd", bank);

    memory->addChild(bank);
  }

  n = scandir(".", &dirlist, selectdir, alphasort);
  popd();

  if (n < 0)
    return;

  for (int i = 0; i < n; i++)
  {
    add_memory_bank(dirlist[i]->d_name, path + "/" + name, core);
    free(dirlist[i]);
  }
  free(dirlist);
}


static void scan_devtree_memory_powernv(hwNode & core)
{
  struct dirent **namelist;
  int n;
  string path = DEVICETREEVPD;

  pushd(DEVICETREEVPD);
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();

  if (n < 0)
    return;

  for (int i = 0; i < n; i++)
  {
    add_memory_bank(namelist[i]->d_name, path, core);
    free(namelist[i]);
  }

  free(namelist);
}


// older POWER hardware
static void scan_devtree_memory_ibm(hwNode & core)
{
  struct dirent **namelist;
  pushd(DEVICETREE);
  int n = scandir(".", &namelist, selectdir, alphasort);
  popd();

  if (n < 0)
    return;

  for (int i = 0; i < n; i++)
  {
    if (strncmp(namelist[i]->d_name, "memory-controller@", 18) == 0)
    {
      add_memory_bank(namelist[i]->d_name, DEVICETREE, core);
    }
    free(namelist[i]);
  }
  free(namelist);
}


// Apple and ARM
static void scan_devtree_memory(hwNode & core)
{
  int currentmc = -1;                             // current memory controller
  hwNode *memory = core.getChild("memory");

  while (true)
  {
    char buffer[10];
    string mcbase;
    vector < string > slotnames;
    vector < string > dimmtypes;
    vector < string > dimmspeeds;
    vector < reg_entry > regs;
    string dimminfo;

    snprintf(buffer, sizeof(buffer), "%d", currentmc);
    if (currentmc >= 0)
      mcbase = string(DEVICETREE "/memory@") + string(buffer);
    else
      mcbase = string(DEVICETREE "/memory");
    slotnames =
      get_strings(mcbase + string("/slot-names"), 4);
    dimmtypes = get_strings(mcbase + string("/dimm-types"));
    dimmspeeds = get_strings(mcbase + string("/dimm-speeds"));
    regs = get_reg_property(mcbase);
    dimminfo = mcbase + string("/dimm-info");

    if (slotnames.size() == 0)
    {
      if (currentmc < 0)
      {
        currentmc++;
        continue;
      }
      else
        break;
    }

    if (!memory || (currentmc > 0))
    {
      memory = core.addChild(hwNode("memory", hw::memory));
    }

    if (!memory)
      break;

    if (regs.size() == slotnames.size())
    {
      for (unsigned int i = 0; i < slotnames.size(); i++)
      {
        uint64_t size = regs[i].size;
        hwNode bank("bank", hw::memory);

	// Parse Memory SPD data
        add_memory_bank_spd(dimminfo, bank);

        if (size > 0)
          bank.addHint("icon", string("memory"));
        bank.setDescription("Memory bank");
        bank.setSlot(slotnames[i]);
        if (i < dimmtypes.size())
          bank.setDescription(dimmtypes[i]);
        if (i < dimmspeeds.size())
          bank.setProduct(hw::strip(dimmspeeds[i]));
        bank.setSize(size);
        memory->addChild(bank);
      }
    }
    currentmc++;

    memory = NULL;
  }
}


struct pmac_mb_def
{
  const char *model;
  const char *modelname;
  const char *icon;
};

static struct pmac_mb_def pmac_mb_defs[] =
{
/*
 * Warning: ordering is important as some models may claim
 * * beeing compatible with several types
 */
  {"AAPL,8500", "PowerMac 8500/8600", ""},
  {"AAPL,9500", "PowerMac 9500/9600", ""},
  {"AAPL,7200", "PowerMac 7200", ""},
  {"AAPL,7300", "PowerMac 7200/7300", ""},
  {"AAPL,7500", "PowerMac 7500", ""},
  {"AAPL,ShinerESB", "Apple Network Server", ""},
  {"AAPL,e407", "Alchemy", ""},
  {"AAPL,e411", "Gazelle", ""},
  {"AAPL,3400/2400", "PowerBook 3400", "laptop"},
  {"AAPL,3500", "PowerBook 3500", "laptop"},
  {"AAPL,Gossamer", "PowerMac G3 (Gossamer)", ""},
  {"AAPL,PowerMac G3", "PowerMac G3 (Silk)", ""},
  {"AAPL,PowerBook1998", "PowerBook Wallstreet", "laptop"},
  {"iMac,1", "iMac (first generation)", ""},
  {"PowerMac1,1", "Blue & White G3", "powermac"},
  {"PowerMac1,2", "PowerMac G4 PCI Graphics", "powermac"},
  {"PowerMac2,1", "iMac FireWire", ""},
  {"PowerMac2,2", "iMac FireWire", ""},
  {"PowerMac3,1", "PowerMac G4 AGP Graphics", "powermac"},
  {"PowerMac3,2", "PowerMac G4 AGP Graphics", "powermac"},
  {"PowerMac3,3", "PowerMac G4 AGP Graphics", "powermac"},
  {"PowerMac3,4", "PowerMac G4 QuickSilver", "powermac"},
  {"PowerMac3,5", "PowerMac G4 QuickSilver", "powermac"},
  {"PowerMac3,6", "PowerMac G4 Windtunnel", "powermac"},
  {"PowerMac4,1", "iMac \"Flower Power\"", ""},
  {"PowerMac4,2", "iMac LCD 15\"", ""},
  {"PowerMac4,4", "eMac", ""},
  {"PowerMac4,5", "iMac LCD 17\"", ""},
  {"PowerMac5,1", "PowerMac G4 Cube", ""},
  {"PowerMac5,2", "PowerMac G4 Cube", ""},
  {"PowerMac6,1", "iMac LCD 17\"", ""},
  {"PowerMac7,2", "PowerMac G5", "powermacg5"},
  {"PowerMac7,3", "PowerMac G5", "powermacg5"},
  {"PowerMac8,1", "iMac G5", ""},
  {"PowerMac8,2", "iMac G5", ""},
  {"PowerMac10,1", "Mac mini", "mini"},
  {"PowerMac10,2", "Mac mini", "mini"},
  {"PowerMac11,2", "PowerMac G5", "powermacg5"},
  {"PowerMac12,1", "iMac G5", ""},
  {"PowerBook1,1", "PowerBook 101 (Lombard)", "laptop"},
  {"PowerBook2,1", "iBook (first generation)", "laptop"},
  {"PowerBook2,2", "iBook FireWire", "laptop"},
  {"PowerBook3,1", "PowerBook Pismo", "laptop"},
  {"PowerBook3,2", "PowerBook Titanium", "laptop"},
  {"PowerBook3,3", "PowerBook Titanium w/ Gigabit Ethernet", "laptop"},
  {"PowerBook3,4", "PowerBook Titanium w/ DVI", "laptop"},
  {"PowerBook3,5", "PowerBook Titanium 1GHz", "laptop"},
  {"PowerBook4,1", "iBook 12\" (May 2001)", "laptop"},
  {"PowerBook4,2", "iBook 2", "laptop"},
  {"PowerBook4,3", "iBook 2 rev. 2 (Nov 2002)", "laptop"},
  {"PowerBook4,4", "iBook 2 rev. 2", "laptop"},
  {"PowerBook5,1", "PowerBook G4 17\"", "laptop"},
  {"PowerBook5,2", "PowerBook G4 15\"", "laptop"},
  {"PowerBook5,3", "PowerBook G4 17\" 1.33GHz", "laptop"},
  {"PowerBook5,4", "PowerBook G4 15\" 1.5/1.33GHz", "laptop"},
  {"PowerBook5,5", "PowerBook G4 17\" 1.5GHz", "laptop"},
  {"PowerBook5,6", "PowerBook G4 15\" 1.67/1.5GHz", "laptop"},
  {"PowerBook5,7", "PowerBook G4 17\" 1.67GHz", "laptop"},
  {"PowerBook5,8", "PowerBook G4 15\" double layer SD", "laptop"},
  {"PowerBook5,9", "PowerBook G4 17\" double layer SD", "laptop"},
  {"PowerBook6,1", "PowerBook G4 12\"", "laptop"},
  {"PowerBook6,2", "PowerBook G4 12\" DVI", "laptop"},
  {"PowerBook6,3", "iBook G4", "laptop"},
  {"PowerBook6,4", "PowerBook G4 12\"", "laptop"},
  {"PowerBook6,5", "iBook G4", "laptop"},
  {"PowerBook6,7", "iBook G4", "laptop"},
  {"PowerBook6,8", "PowerBook G4 12\" 1.5GHz", "laptop"},
  {"RackMac1,1", "XServe", ""},
  {"RackMac1,2", "XServe rev. 2", ""},
  {"RackMac3,1", "XServe G5", ""},
};

static bool get_apple_model(hwNode & n)
{
  string model = n.getProduct();
  if (model == "")
    return false;

  for (unsigned int i = 0; i < sizeof(pmac_mb_defs) / sizeof(pmac_mb_def);
    i++)
  if (model == pmac_mb_defs[i].model)
  {
    n.setProduct(pmac_mb_defs[i].modelname);
    n.addHint("icon", string(pmac_mb_defs[i].icon));
  }

  return false;
}

struct ibm_model_def {
  const char *model;
  const char *modelname;
  const char *icon;
  const char *chassis; // matches DMI chassis types
};
struct ibm_model_def ibm_model_defs[] =
{
  {"0200", "BladeCenter QS20", "", "blade"},
  {"0792", "BladeCenter QS21", "", "blade"},
  {"6778", "BladeCenter JS21", "", "blade"},
  {"6779", "BladeCenter JS21", "", "blade"},
  {"7047-185", "IntelliStation POWER 185", "towercomputer", "tower"},
  {"7988", "BladeCenter JS21", "", "blade"},
  {"8202", "Power 720 Express", "", "rackmount"},
  {"8205", "Power 740 Express", "", "rackmount"},
  {"8231-E1C", "Power 710 Express", "", "rackmount"},
  {"8231-E1D", "Power 710 Express", "", "rackmount"},
  {"8231-E2C", "Power 730 Express", "", "rackmount"},
  {"8231-E2D", "Power 730 Express", "", "rackmount"},
  {"8233-E8B", "Power 750 Express", "", "rackmount"},
  {"8236-E8C", "Power 755", "", "rackmount"},
  {"8286-41A", "Power Systems S814", "", "rackmount"},
  {"8286-42A", "Power Systems S824", "", "rackmount"},
  {"8406-70Y", "Power PS700", "", "rackmount"},
  {"8406-71Y", "BladeCenter PS702", "", "blade"},
  {"8842", "BladeCenter JS20", "", "blade"},
  {"8844", "BladeCenter JS21", "", "blade"},
  {"9111-285", "IntelliStation POWER 285", "towercomputer", "tower"},
  {"9112-265", "IntelliStation POWER 265", "towercomputer", "tower"},
  {"9114-275", "IntelliStation POWER 275", "towercomputer", "tower"},
  {"9123", "eServer OpenPower 710", "", "rackmount"},
  {"9124", "eServer OpenPower 720", "", "rackmount"},
};

static void get_ips_model(hwNode & n)
{
  string product = n.getProduct();
  if (product.empty())
    return;
  if (product.compare(0, 4, "IPS,") != 0)
    return;

  n.setVendor("IPS");
}

static void get_ibm_model(hwNode & n)
{
  string product = n.getProduct();
  if (product.empty())
    return;
  if (product.compare(0, 4, "IBM,") != 0)
    return;

  n.setVendor("IBM");
  string machinetype = product.substr(4, 4);
  string model = product.substr(4);

  for (size_t i = 0; i < sizeof(ibm_model_defs) / sizeof(ibm_model_def); i ++)
  {
    if (ibm_model_defs[i].model == machinetype || ibm_model_defs[i].model == model)
    {
      n.setProduct(n.getProduct() + " (" + ibm_model_defs[i].modelname + ")");
      n.addHint("icon", string(ibm_model_defs[i].icon));
      n.setConfig("chassis", ibm_model_defs[i].chassis);
      return;
    }
  }
}

static void fix_serial_number(hwNode & n)
{
  string serial = n.getSerial();

  if(serial.find('\0')==string::npos) return;     // nothing to do

  n.setSerial(hw::strip(serial.substr(13)) + hw::strip(serial.substr(0,13)));
}


bool scan_device_tree(hwNode & n)
{
  hwNode *core = n.getChild("core");

  if (!exists(DEVICETREE))
    return false;

  if (!core)
  {
    n.addChild(hwNode("core", hw::bus));
    core = n.getChild("core");
  }

  if (exists(DEVICETREE "/ibm,vendor-model"))
	  n.setProduct(get_string(DEVICETREE "/ibm,vendor-model", n.getProduct()));
  else
	  n.setProduct(get_string(DEVICETREE "/model", n.getProduct()));

  n.addHint("icon", string("motherboard"));

  n.setSerial(get_string(DEVICETREE "/serial-number", n.getSerial()));
  if (n.getSerial() == "")
  {
	  if (exists(DEVICETREE "/ibm,vendor-system-id"))
		  n.setSerial(get_string(DEVICETREE "/ibm,vendor-system-id"));
	  else
		  n.setSerial(get_string(DEVICETREE "/system-id"));
  }
  fix_serial_number(n);

  n.setVendor(get_string(DEVICETREE "/copyright", n.getVendor()));
  get_apple_model(n);
  get_ips_model(n);
  get_ibm_model(n);
  if (matches(get_string(DEVICETREE "/compatible"), "^ibm,powernv"))
  {
    n.setVendor(get_string(DEVICETREE "/vendor", "IBM"));

    if (exists(DEVICETREE "/model-name"))
      n.setProduct(n.getProduct() + " (" +
		   hw::strip(get_string(DEVICETREE "/model-name")) + ")");

    n.setDescription("PowerNV");
    if (core)
    {
      core->addHint("icon", string("board"));
      scan_devtree_root(*core);
      scan_devtree_cpu_power(*core);
      scan_devtree_memory_powernv(*core);
      scan_devtree_firmware_powernv(*core);
      n.addCapability("powernv", "Non-virtualized");
      n.addCapability("opal", "OPAL firmware");
    }
  }
  else if(matches(get_string(DEVICETREE "/compatible"), "qemu,pseries"))
  {
    string product;

    if ( exists(DEVICETREE "/host-serial") )
      n.setSerial(get_string(DEVICETREE "/host-serial"));

    if ( exists( DEVICETREE "/vm,uuid") )
      n.setConfig("uuid", get_string(DEVICETREE "/vm,uuid"));

    n.setVendor(get_string(DEVICETREE "/vendor", "IBM"));

    if ( exists(DEVICETREE "/hypervisor/compatible") ) {
      product = get_string(DEVICETREE "/hypervisor/compatible");
      product = product.substr(0, product.size()-1);
    }

    if ( exists(DEVICETREE "/host-model") ) {
      product += " Model# ";
      product += get_string(DEVICETREE "/host-model");
    }

    if (product != "")
      n.setProduct(product);

    n.setDescription("pSeries Guest");

    if (core)
    {
      core->addHint("icon", string("board"));
      scan_devtree_root(*core);
      scan_devtree_cpu_power(*core);
      core->addCapability("qemu", "QEMU virtualization");
      core->addCapability("guest", "Virtualization guest");
    }
  }
  else
  {
    if (core)
    {
      core->addHint("icon", string("board"));
      scan_devtree_root(*core);
      scan_devtree_bootrom(*core);
      if (exists(DEVICETREE "/ibm,lpar-capable")) {
        n.setDescription("pSeries LPAR");
        if (exists( DEVICETREE "/ibm,partition-uuid"))
          n.setConfig("uuid", get_string(DEVICETREE "/ibm,partition-uuid"));
        scan_devtree_cpu_power(*core);
      }
      else {
        if (exists(DEVICETREE "/cpus"))
          scan_devtree_cpu(*core);
      }
      scan_devtree_memory(*core);
      scan_devtree_memory_ibm(*core);
    }
  }

  return true;
}

void add_device_tree_info(hwNode & n, string sysfs_path)
{
  string of_node = sysfs_path + "/of_node";
  string val;

  if (!exists(of_node))
    return;

  /* read location / slot data */
  val = hw::strip(get_string(of_node + "/ibm,loc-code", ""));
  if (val == "")
    val = hw::strip(get_string(of_node + "/ibm,slot-location-code", ""));
  if (val == "")
    val = hw::strip(get_string(of_node + "/ibm,slot-label"));

  if (val != "")
    n.setSlot(val);
}
