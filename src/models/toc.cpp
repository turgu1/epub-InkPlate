// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __TOC__ 1
#include "models/toc.hpp"

#include "models/epub.hpp"


std::string 
TOC::build_filename()
{
  std::string epub_fname = epub.get_current_filename();
  return epub_fname.substr(0, epub_fname.find_last_of('.')) + ".toc";
}

bool
TOC::load()
{
  std::string filename = build_filename();
  clean();

  LOG_D("Reading toc: %s.", filename.c_str());

  if (!db.open(filename)) {
    LOG_E("Can't open toc: %s", filename.c_str());
    return false;
  }

  // We first verify if the database content is of the current version

  bool version_ok = false;
  VersionRecord version_record;

  if (db.get_record_count() > 0) {
    db.goto_first();
    if (db.get_record_size() == sizeof(version_record)) {
      db.get_record(&version_record, sizeof(version_record));
      if ((version_record.version == TOC_DB_VERSION) &&
          (strcmp(version_record.app_name, TOC_NAME) == 0)) {
        version_ok = true;
      }
    }
  }

  if (!version_ok) {
    LOG_E("Toc is of a wrong version or is empty");
    db.close();
    return false;
  }

  if (db.goto_next()) {
    char_buffer_size = db.get_record_size();
    if (char_buffer_size > 0) {
      char_buffer = (char *) allocate(char_buffer_size);
      if (db.get_record(char_buffer, char_buffer_size)) {
        uint16_t count = db.get_record_count() - 2;
        if (count > 0) {
          entries.resize(count);
          uint16_t idx = 0;
          while ((idx < count) && db.goto_next()) {
            if (db.get_record_size() == sizeof(EntryRecord)) {
              if (!db.get_record(&entries[idx], sizeof(EntryRecord))) break;
              entries[idx].label = char_buffer + (size_t)entries[idx].label;
              idx++;
            }
            else {
              LOG_E("DB corrupted.");
              break;
            }
          }
          if (idx != count) {
            LOG_E("The toc has been partially read: %d records.", idx);
          }
          else {
            ready = compacted = saved = true;
          }
        }
        else {
          LOG_E("DB empty.");
        }
      }
      else {
        LOG_E("Unable to get char buffer content.");
      }
    }
    else {
      LOG_E("Toc db seems to be empty.");
    }
  }
  else {
    LOG_E("Toc db is empty.");
  }

  db.close();

  #if DEBUGGING
    show();
  #endif

  if (ready) LOG_I("Reading toc completed. Entry count: %d.", entries.size());
  return ready;
}

bool
TOC::save()
{
  if (saved) return true;

  std::string filename = build_filename();
  if (!compact()) return false;

  if (db.create(filename)) {
    VersionRecord version_record;
    strcpy(version_record.app_name, TOC_NAME);
    version_record.version = TOC_DB_VERSION;

    if (db.add_record(&version_record, sizeof(VersionRecord))) {
      if (db.add_record(char_buffer, char_buffer_size)) {
        uint16_t idx;
        for (idx = 0; idx < entries.size(); idx++) {
          EntryRecord e = entries[idx];
          e.label = e.label - (size_t) char_buffer;
          if (!db.add_record(&e, sizeof(EntryRecord))) {
            LOG_E("Unable to add entry record.");
            break;
          }
        }
        ready = saved = (idx == entries.size());
      }
      else {
        LOG_E("Unable to save char_buffer.");
      }
    }
    else {
      LOG_E("Unable to save version record.");
    }
  }
  else {
    LOG_E("Unable to create toc db.");
  }

  db.close();

  #if DEBUGGING
    show();
  #endif

  return saved;
}

unsigned char bin(char ch);

void
TOC::clean_filename(char * fname)
{
  char * s = fname;
  char * d = fname;

  while (*s) {
    if (*s == '%') {
      *d++ = (bin(s[1]) << 4) + bin(s[2]);
      s += 3;
    }
    else {
      *d++ = *s++;
    }
  }

  *d = 0;
}

bool
TOC::do_nav_points(pugi::xml_node & node, uint8_t level)
{
  xml_attribute attr;

  do {
    const char * label = nullptr;
    const char * fname = nullptr;

    xml_node n;

    if ((n = node.child("navLabel")) &&
        (n =    n.child("text"))) {      
      label = n.text().as_string();            
    }
    if ((n = node.child("content")) &&
        (attr = n.attribute("src"))) {
      fname = attr.value();
    }

    EntryRecord  entry;
    std::string  the_id;
    char       * filename_to_find;

    entry.label = char_pool->allocate(strlen(label) + 1);
    strcpy(entry.label, label);
    entry.page_id = PageLocs::PageId(0, -1);
    entry.level   = level;

    const char * hash_pos = strchr(fname, '#');
    if (hash_pos != nullptr) {
      the_id.assign(hash_pos + 1);
      filename_to_find = char_pool->allocate(hash_pos - fname + 1);
      strlcpy(filename_to_find, fname, hash_pos - fname + 1);
    }
    else {
      the_id.clear();
      filename_to_find = char_pool->allocate(strlen(fname) + 1);
      strcpy(filename_to_find, fname);
    }

    clean_filename(filename_to_find);

    if ((n = opf->child("package")) &&
        (n =    n.child("manifest")) &&
        (n =    n.child("item"))) {
      // Search in the manifest to find the entry associated with the filename
      do {
        if ((attr = n.attribute("href")) &&
            (strcmp(attr.value(), filename_to_find) == 0)) {
          break;
        }
        n = n.next_sibling();
      } while (n);
      // Is the node with the same filename is found, we then search the
      // itemref in the spine with the same id associated with the item.
      if (n && (attr = n.attribute("id"))) {
        const char * idref = attr.value();

        if ((n = opf->child("package")) &&
            (n =    n.child("spine"  )) &&
            (n =    n.child("itemref"))) {
          int16_t index = 0;
          while (n && 
                  ((attr = n.attribute("idref"))) &&
                  (strcmp(attr.value(), idref) != 0)) {
            n = n.next_sibling();
            index++;
          }
          if (n) {
            entry.page_id.itemref_index = index;
            if (!the_id.empty()) {
              infos.insert(std::make_pair(
                std::make_pair(index, the_id),
                (int16_t)entries.size()
              ));
              some_ids = true;
            }
            else {
              entry.page_id.offset = 0;
            }
            entries.push_back(entry);
          }
          else {
            LOG_E("Unable to find reference %s in spine", idref);
            return false;
          }
        }
        else {
          LOG_E("No spine in OPF!!");
          return false;
        }
      }
      else {
        LOG_E("NCX inconsistency.");
        return false;
      }
    }
    else {
      LOG_E("OPF inconsistency.");
      return false;
    }

    if ((n = node.child("navPoint"))) {
      if (!do_nav_points(n, level + 1)) return false;
    }
    
    node = node.next_sibling();
  } while (node);

  return true;
}

bool
TOC::load_from_epub()
{
  
  xml_node                   node;
  xml_attribute              attr;
  const char *               filename = nullptr;

  opf = &epub.get_opf();

  clean();

  // Retrieve the ncx filename

  if ((node =  opf->child("package" )) &&
      (node = node.child("manifest")) &&
      (node = node.child("item"    ))) {
    do {
      if ((attr = node.attribute("id")) &&
          (strcmp(attr.value(), "ncx") == 0)) {
        if ((attr = node.attribute("href"))) {
          filename = attr.value();
        }
        break;
      }
      node = node.next_sibling();
    } while (node);
  }
  else return false;
  if (filename == nullptr) return false;

  // retrieve the ncx file data

  uint32_t  ncx_size;
  bool      result = false;

  if ((ncx_data = epub.retrieve_file(filename, ncx_size)) != nullptr) {
    if ((ncx_opf = new pugi::xml_document()) == nullptr) {
      free(ncx_data);
      return false;
    }
  }
  else return false;

  // parse xml and load navPoint entries

  xml_parse_result res = ncx_opf->load_buffer_inplace(ncx_data, ncx_size);
  if (res.status != status_ok) {
    LOG_E("xml load error: %d", res.status);
    goto error;
  }
  else {
    if ((node = ncx_opf->child("ncx"     )) &&
        (node =     node.child("navMap"  )) &&
        (node =     node.child("navPoint"))) {

      if (char_pool == nullptr) {
        char_pool = new CharPool;
        if (char_pool == nullptr) goto error;
      }

      if (!do_nav_points(node, 0)) goto error;

      result = !entries.empty();
    }
  }

  goto ok;

error:
  result = false;
ok:
  ncx_opf->reset();
  free(ncx_data);
  ncx_opf  = nullptr;
  ncx_data = nullptr;

  #if DEBUGGING
    show();
    show_info();
  #endif

  return result;
}

bool
TOC::compact()
{
  if (compacted) return true;
  
  if (char_buffer != nullptr) free(char_buffer);
  char_buffer_size = 0;

  for (auto & e : entries) {
    char_buffer_size += strlen(e.label) + 1;
  }

  char_buffer = (char *) allocate(char_buffer_size);
  if (char_buffer == nullptr) return false;

  char * buff = char_buffer;
  for (auto & e : entries) {
    strcpy(buff, e.label);
    e.label = buff;
    buff += strlen(buff) + 1;
  }

  if (char_pool != nullptr) {
    delete char_pool;
    char_pool = nullptr;
  }
  infos.clear();

  compacted = true;
  return true;
}

void
TOC::clean()
{
  if (char_pool   != nullptr) delete char_pool;
  if (char_buffer != nullptr) free(char_buffer);

  if (ncx_opf != nullptr) {
    ncx_opf->reset();
    delete ncx_opf;
    ncx_opf = nullptr;
  }

  if (ncx_data != nullptr) {
    free(ncx_data);
    ncx_data = nullptr;
  }

  char_pool   = nullptr;
  char_buffer = nullptr;

    infos.clear();
  entries.clear();

  ready     = false;
  compacted = false;
  saved     = false;
  some_ids  = false;
}

void 
TOC::set(std::string & id, int32_t current_offset)
{
  int16_t itemref_index = page_locs.get_current_itemref_index();
  Infos::iterator infos_it = infos.find(std::make_pair(itemref_index, id));

  if (infos_it != infos.end()) {
    entries[infos_it->second].page_id.offset = current_offset;
  }
}

void 
TOC::set(int32_t current_offset)
{
  int16_t itemref_index = page_locs.get_current_itemref_index();
  int16_t idx = -1;

  for (auto & e : entries) {
    idx++;
    if (e.page_id.itemref_index == itemref_index) {
      break;
    }
  }
  if ((idx >= 0) && (idx < entries.size())) {   
    entries[idx].page_id.offset = current_offset;
  }
}

#if DEBUGGING

void
TOC::show()
{
  std::cout << "----- Table of Content: -----" << std::endl;

  for (auto & e : entries) {
    std::cout << e.label << " : [" 
              << e.page_id.itemref_index << ", " 
              << e.page_id.offset << "]" << std::endl;
  }

  std::cout << "----- End TOC -----" << std::endl;
}

void
TOC::show_info()
{
  std::cout << "----- TOC Infos -----" << std::endl;

  for (auto & e : infos) {
    std::cout << "Id: " << e.first.second << ", " 
              << "Item index: " << e.first.first << ", "
              << "TOC Entry index: " << e.second << std::endl;
  }

  std::cout << "----- End TOC Infos -----" << std::endl;
}

#endif