/*
 * Copyright 2009-2016 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at:
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 */

//
// symbol tables hold the int to string mapping for ion symbols
// they come in 3 flavors - local, shared, and the system symbol table
//

#include "ion_internal.h"
#include <ctype.h>
#include <string.h>
//#include "hashfn.h"

// strings are commonly used pointer length values
// the referenced data is (should be) immutable
// and is often shared or owned by others
// the character encoding is utf-8 and both comparisons
// and collation is only done as memcmp

struct _ion_symbol_table
{
    void               *owner;          // this may be a reader, writer, catalog or itself
    BOOL                is_locked;
    BOOL                has_local_symbols;
    ION_STRING          name;
    int32_t             version;
    SID                 max_id;         // the max SID of this symbol tables symbols, including shared symbols.
    SID                 min_local_id;   // the lowest local SID. Only valid if has_local_symbols is TRUE. by_id[0] holds this symbol.
    SID                 flushed_max_id; // the max SID already serialized. If symbols are appended, only the ones after this need to be serialized.
    ION_COLLECTION      import_list;    // collection of ION_SYMBOL_TABLE_IMPORT
    ION_COLLECTION      symbols;        // collection of ION_SYMBOL
    ION_SYMBOL_TABLE   *system_symbol_table;

    int32_t             by_id_max;      // current size of by_id, which holds the local symbols, but NOT necessarily the number of declared local symbols.
    ION_SYMBOL        **by_id;          // the local symbols. Accessing shared symbols requires delegate lookups to the imports.
    ION_INDEX           by_name;        // the local symbols (by name).

};

iERR _ion_symbol_table_local_find_by_sid(ION_SYMBOL_TABLE *symtab, SID sid, ION_SYMBOL **p_sym);

iERR ion_symbol_table_open(hSYMTAB *p_hsymtab, hOWNER owner)
{
    return ion_symbol_table_open_with_type(p_hsymtab, owner, ist_LOCAL);
}

iERR ion_symbol_table_open_with_type(hSYMTAB *p_hsymtab, hOWNER owner, ION_SYMBOL_TABLE_TYPE type) {
    iENTER;
    ION_SYMBOL_TABLE *table, *system;

    if (p_hsymtab == NULL) {
        FAILWITH(IERR_INVALID_ARG);
    }

    IONCHECK(_ion_symbol_table_get_system_symbol_helper(&system, ION_SYSTEM_VERSION));
    switch (type) {
        case ist_LOCAL:
            IONCHECK(_ion_symbol_table_open_helper(&table, owner, system));
            break;
        case ist_SHARED:
            IONCHECK(_ion_symbol_table_open_helper(&table, owner, NULL));
            table->system_symbol_table = system;
            break;
        default:
            FAILWITH(IERR_INVALID_ARG);
    }

    *p_hsymtab = PTR_TO_HANDLE(table);

    iRETURN;
}

iERR _ion_symbol_table_open_helper(ION_SYMBOL_TABLE **p_psymtab, hOWNER owner, ION_SYMBOL_TABLE *system)
{
    iENTER;
    ION_SYMBOL_TABLE      *symtab;

    if (owner == NULL) {
        symtab = (ION_SYMBOL_TABLE *)ion_alloc_owner(sizeof(*symtab));
        owner = symtab;
    }
    else {
        symtab = (ION_SYMBOL_TABLE *)ion_alloc_with_owner(owner, sizeof(*symtab));
    }
    if (symtab == NULL) FAILWITH(IERR_NO_MEMORY);

    memset(symtab, 0, sizeof(*symtab));

    symtab->system_symbol_table = system;
    symtab->owner = owner;

    _ion_collection_initialize(owner, &symtab->import_list, sizeof(ION_SYMBOL_TABLE_IMPORT)); // collection of ION_SYMBOL_TABLE_IMPORT
    _ion_collection_initialize(owner, &symtab->symbols, sizeof(ION_SYMBOL)); // collection of ION_SYMBOL

    // if there is a system table to work from (there isn't when we
    // create the system symbol table) we need to copy the system
    // symbols to seed our symbol list
    if (system) {
        IONCHECK(_ion_symbol_table_local_incorporate_symbols(symtab, NULL, system->max_id));
    }
    *p_psymtab = symtab;

    iRETURN;
}

iERR ion_symbol_table_clone(hSYMTAB hsymtab, hSYMTAB *p_hclone)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab, *clone,*system;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_hclone == NULL) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_system_symbol_helper(&system, ION_SYSTEM_VERSION));
    IONCHECK(_ion_symbol_table_clone_with_owner_helper(&clone, symtab, symtab->owner, system));

    *p_hclone = PTR_TO_HANDLE(clone);

    iRETURN;
}

iERR ion_symbol_table_clone_with_owner(hSYMTAB hsymtab, hSYMTAB *p_hclone, hOWNER owner)
{
    iENTER;
    ION_SYMBOL_TABLE *orig, *clone, *system;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_hclone == NULL) FAILWITH(IERR_INVALID_ARG);

    orig = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_system_symbol_helper(&system, ION_SYSTEM_VERSION));
    IONCHECK(_ion_symbol_table_clone_with_owner_helper(&clone, orig, owner, system));

    *p_hclone = PTR_TO_HANDLE(clone);

    iRETURN;
}

iERR _ion_symbol_table_clone_with_owner_and_system_table(hSYMTAB hsymtab, hSYMTAB *p_hclone, hOWNER owner, hSYMTAB hsystem)
{
    iENTER;
    ION_SYMBOL_TABLE *orig, *clone, *system;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_hclone == NULL) FAILWITH(IERR_INVALID_ARG);

    orig = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
    system = HANDLE_TO_PTR(hsystem, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_clone_with_owner_helper(&clone, orig, owner, system));

    *p_hclone = PTR_TO_HANDLE(clone);

    iRETURN;
}

iERR _ion_symbol_table_clone_with_owner_helper(ION_SYMBOL_TABLE **p_pclone, ION_SYMBOL_TABLE *orig, hOWNER owner, ION_SYMBOL_TABLE *system)
{
    iENTER;
    ION_SYMBOL_TABLE       *clone;
    BOOL                    new_owner, is_shared;
    ION_SYMBOL             *p_symbol;
    ION_COLLECTION_CURSOR   symbol_cursor;
    ION_COPY_FN             copy_fn;
    ION_SYMBOL_TABLE_TYPE   type;

    ASSERT(orig != NULL);
    ASSERT(p_pclone != NULL);

    IONCHECK(_ion_symbol_table_get_type_helper(orig, &type));
    switch(type) {
    default:
    case ist_EMPTY:
        FAILWITH(IERR_INVALID_STATE);
    case ist_LOCAL:
        IONCHECK(_ion_symbol_table_open_helper(&clone, owner, system));
        is_shared = FALSE;
        break;
    case ist_SYSTEM:  // system symbol tables are considered shared tables
    case ist_SHARED:
        // we don't copy the system symbols into shared tables
        IONCHECK(_ion_symbol_table_open_helper(&clone, owner, NULL));
        is_shared = TRUE;
        break;
    }   

    clone->max_id = orig->max_id;
    clone->min_local_id = orig->min_local_id;
    clone->system_symbol_table = orig->system_symbol_table;

    // since these value should be immutable if the owner
    // has NOT changed we can use cheaper copies
    new_owner = (orig->owner != clone->owner);
    if (is_shared) {
        // if this is a shared table we copy the name and version
        clone->version = orig->version;
        if (new_owner) {
            // otherwise we have to do expensive copies
            IONCHECK(ion_string_copy_to_owner(clone->owner, &clone->name, &orig->name));
        }
        else {
            // we get to share the name contents
            ION_STRING_ASSIGN(&clone->name, &orig->name);
        }
    }

    // now we move the imports
    copy_fn = new_owner ? _ion_symbol_table_local_import_copy_new_owner 
                        : _ion_symbol_table_local_import_copy_same_owner;
    IONCHECK(_ion_collection_copy(&clone->import_list, &orig->import_list, copy_fn, clone->owner));

    // and finally copy the actual symbols
    copy_fn = new_owner ? _ion_symbol_local_copy_new_owner 
                        : _ion_symbol_local_copy_same_owner;
    IONCHECK(_ion_collection_copy(&clone->symbols, &orig->symbols, copy_fn, clone->owner));

    // now adjust the symbol table owner handles (hsymtab)
    ION_COLLECTION_OPEN(&clone->symbols, symbol_cursor);
    for (;;) {
        ION_COLLECTION_NEXT(symbol_cursor, p_symbol);
        if (!p_symbol) break;
    }
    ION_COLLECTION_CLOSE(symbol_cursor);

    *p_pclone = clone;

    iRETURN;
}

iERR ion_symbol_table_get_system_table(hSYMTAB *p_hsystem_table, int32_t version)
{
    iENTER;
    ION_SYMBOL_TABLE *system;

    if (p_hsystem_table == NULL) FAILWITH(IERR_INVALID_ARG);
    if (version != 1)            FAILWITH(IERR_INVALID_ION_VERSION);

    IONCHECK(_ion_symbol_table_get_system_symbol_helper(&system, version));
    *p_hsystem_table = PTR_TO_HANDLE(system);

    iRETURN;
}

// HACK - TODO - hate this
static THREAD_LOCAL_STORAGE ION_SYMBOL_TABLE *p_system_symbol_table_version_1 = NULL;

iERR _ion_symbol_table_get_system_symbol_helper(ION_SYMBOL_TABLE **pp_system_table, int32_t version)
{
    iENTER;

    ASSERT( pp_system_table != NULL );
    ASSERT( version == 1 ); // only one we understand at this point

    if (!p_system_symbol_table_version_1) {
        IONCHECK(_ion_symbol_table_local_make_system_symbol_table_helper(version));
    }
    *pp_system_table = p_system_symbol_table_version_1;

    iRETURN;
}


// currently the system symbol table uses 1304 bytes or so
#define kIonSystemSymbolMemorySize 2048
static THREAD_LOCAL_STORAGE char gSystemSymbolMemory[kIonSystemSymbolMemorySize];

void* smallLocalAllocationBlock()
{
    ION_ALLOCATION_CHAIN *new_block = (ION_ALLOCATION_CHAIN*)gSystemSymbolMemory;
    SIZE                  alloc_size = kIonSystemSymbolMemorySize;

    new_block->size     = alloc_size;

    new_block->next     = NULL;
    new_block->head     = NULL;

    new_block->position = ION_ALLOC_BLOCK_TO_USER_PTR(new_block);
    new_block->limit    = ((BYTE*)new_block) + alloc_size;

    return new_block->position;
}

// HACK - hate this
iERR _ion_symbol_table_local_make_system_symbol_table_helper(int32_t version)
{
    iENTER;
    ION_SYMBOL_TABLE      *psymtab;
    hOWNER sysBlock;

    ASSERT( version == 1 ); // only one we understand at this point
    ASSERT(p_system_symbol_table_version_1 == NULL);
    
    // need a SMALL block for the system symbol table
    sysBlock = smallLocalAllocationBlock();
    
    IONCHECK(_ion_symbol_table_open_helper(&psymtab, sysBlock, NULL));

    psymtab->version = version;
    ION_STRING_ASSIGN(&psymtab->name, &ION_SYMBOL_ION_STRING);
    psymtab->system_symbol_table = psymtab; // the system symbol table is it's own system symbol table (hmmm)

    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_ION_STRING, ION_SYS_SID_ION, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_VTM_STRING, ION_SYS_SID_IVM, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_SYMBOL_TABLE_STRING, ION_SYS_SID_SYMBOL_TABLE, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_NAME_STRING, ION_SYS_SID_NAME, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_VERSION_STRING, ION_SYS_SID_VERSION, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_IMPORTS_STRING, ION_SYS_SID_IMPORTS, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_SYMBOLS_STRING, ION_SYS_SID_SYMBOLS, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_MAX_ID_STRING, ION_SYS_SID_MAX_ID, NULL));
    IONCHECK(_ion_symbol_table_local_add_symbol_helper(psymtab, &ION_SYMBOL_SHARED_SYMBOL_TABLE_STRING, ION_SYS_SID_SHARED_SYMBOL_TABLE, NULL));

    IONCHECK(_ion_symbol_table_lock_helper(psymtab));

    // TODO: THREADING - THIS ASSIGNMENT NEEDS TO BE MAKE THREAD SAFE !!
    // but we only need 1 copy of each system symbol table (the system symbol table)
    p_system_symbol_table_version_1 = psymtab;

    iRETURN;
}

iERR _ion_symbol_table_local_load_import_list(ION_READER *preader, hOWNER owner, ION_COLLECTION *pimport_list)
{
    iENTER;
    ION_SYMBOL_TABLE_IMPORT *import;
    ION_TYPE   type;
    ION_STRING str;
    SID        fld_sid;

    ASSERT(preader->_catalog != NULL);

    ION_STRING_INIT(&str);

    IONCHECK(_ion_reader_step_in_helper(preader));
    for (;;) {
        IONCHECK(_ion_reader_next_helper(preader, &type));
        if (type == tid_EOF) break;
        if (type != tid_STRUCT) continue;

        import = (ION_SYMBOL_TABLE_IMPORT *)_ion_collection_append(pimport_list);
        memset(import, 0, sizeof(ION_SYMBOL_TABLE_IMPORT));
        import->descriptor.max_id = ION_SYS_SYMBOL_MAX_ID_UNDEFINED;

        // step into the import struct
        IONCHECK(_ion_reader_step_in_helper(preader));
        for (;;) {
            IONCHECK(_ion_reader_next_helper(preader, &type));
            if (type == tid_EOF) break;

            IONCHECK(_ion_symbol_table_get_field_sid_force(preader, &fld_sid));
            switch(fld_sid) {
            case ION_SYS_SID_NAME:     /* "name" */
                if (!ION_STRING_IS_NULL(&import->descriptor.name)) FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "too many names in import list");
                if (type == tid_STRING) {
                    IONCHECK(_ion_reader_read_string_helper(preader, &str));
                    IONCHECK(ion_string_copy_to_owner(owner, &import->descriptor.name, &str));
                }
                break;
            case ION_SYS_SID_VERSION:  /* "version" */
                if (import->descriptor.version) FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "too many versions in import list");
                if (type == tid_INT) {
                    IONCHECK(_ion_reader_read_int32_helper(preader, &import->descriptor.version));
                }
                break;
            case ION_SYS_SID_MAX_ID:   /* "max_id" */
                // Edge case: the import contains n max_id declarations, and the first x <= n are explicitly -1. In this
                // case, the following line won't trigger a failure. However, the spec doesn't clearly define what
                // implementations must do when multiple of the same field is encountered in an import, so it doesn't
                // seem worth it to address this now.
                if (import->descriptor.max_id != ION_SYS_SYMBOL_MAX_ID_UNDEFINED) FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "too many max_id fields in import list");
                BOOL is_null;
                IONCHECK(ion_reader_is_null(preader, &is_null));
                if (type == tid_INT && !is_null) {
                    IONCHECK(_ion_reader_read_int32_helper(preader, &import->descriptor.max_id));
                }
                break;
            default:
                break;
            }
        }
        if (import->descriptor.version < 1) {
            import->descriptor.version = 1;
        }

        if (ION_STRING_IS_NULL(&import->descriptor.name)) {
            FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "A shared symbol table must have a name.");
        }

        IONCHECK(_ion_catalog_find_best_match_helper(preader->_catalog, &import->descriptor.name, import->descriptor.version, import->descriptor.max_id, &import->shared_symbol_table));
        // step back out to the list of imports
        IONCHECK(_ion_reader_step_out_helper(preader));
    }
    // step back out to the symbol table struct
    IONCHECK(_ion_reader_step_out_helper(preader));

    iRETURN;
}

iERR _ion_symbol_table_local_load_symbol_list(ION_READER *preader, hOWNER owner, ION_COLLECTION *psymbol_list)
{
    iENTER;

    ION_SYMBOL *sym;
    ION_TYPE    type;
    ION_STRING  str;
    BOOL        is_symbol_null;

    IONCHECK(_ion_reader_step_in_helper(preader));
    for (;;) {
        IONCHECK(_ion_reader_next_helper(preader, &type));
        if (type == tid_EOF) break;

        // NOTE: any value in the symbols list that is null or is not a string is treated as a valid SID mapping with
        // unknown text.

        ION_STRING_INIT(&str);
        IONCHECK(ion_reader_is_null(preader, &is_symbol_null));
        if (type == tid_STRING && !is_symbol_null) {
            IONCHECK(_ion_reader_read_string_helper(preader, &str));
        }

        sym = (ION_SYMBOL *)_ion_collection_append(psymbol_list);

        if (!ION_STRING_IS_NULL(&str)) {
            IONCHECK(ion_string_copy_to_owner(owner, &sym->value, &str));
        }
        else {
            ION_STRING_ASSIGN(&sym->value, &str);
        }
        sym->sid = UNKNOWN_SID;
    }
    // step back out to the symbol table struct
    IONCHECK(_ion_reader_step_out_helper(preader));

    iRETURN;
}

iERR ion_symbol_table_load(hREADER hreader, hOWNER owner, hSYMTAB *p_hsymtab)
{
    iENTER;
    ION_READER       *preader;
    ION_SYMBOL_TABLE *psymtab, *system;


    if (hreader   == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);

    preader = HANDLE_TO_PTR(hreader, ION_READER);

    IONCHECK(_ion_symbol_table_get_system_symbol_helper(&system, ION_SYSTEM_VERSION));
    IONCHECK(_ion_symbol_table_load_helper(preader, owner, system, &psymtab));

    *p_hsymtab = PTR_TO_HANDLE(psymtab);

    iRETURN;
}

iERR _ion_symbol_table_get_field_sid_force(ION_READER *preader, SID *fld_sid)
{
    iENTER;
    SID sid;
    ION_STRING *field_name;

    IONCHECK(_ion_reader_get_field_sid_helper(preader, &sid));
    if (sid <= UNKNOWN_SID) {
        // Binary readers should fail before reaching this point.
        ASSERT(preader->type == ion_type_text_reader);
        IONCHECK(_ion_reader_get_field_name_helper(preader, &field_name));
        if (ION_STRING_IS_NULL(field_name)) {
            FAILWITH(IERR_INVALID_FIELDNAME);
        }
        IONCHECK(_ion_symbol_table_local_find_by_name(preader->_current_symtab->system_symbol_table, field_name, &sid, NULL));
    }

    *fld_sid = sid;
    iRETURN;
}

iERR _ion_symbol_table_append(ION_READER *preader, hOWNER owner, ION_SYMBOL_TABLE *system, ION_COLLECTION *symbols_to_append, ION_SYMBOL_TABLE **p_symtab) {
    iENTER;
    ION_SYMBOL_TABLE *cloned;
    ION_SYMBOL_TABLE_TYPE type;
    ION_COLLECTION_CURSOR symbol_cursor;
    ION_SYMBOL *symbol_to_append, *appended_symbol;

    ASSERT(p_symtab);

    IONCHECK(_ion_symbol_table_get_type_helper(preader->_current_symtab, &type));
    if (type != ist_SYSTEM) {
        ASSERT(type != ist_SHARED);
        // Copy all the symbols and imports of the current symbol table into the new symbol table.
        IONCHECK(_ion_symbol_table_clone_with_owner_helper(&cloned, preader->_current_symtab, owner, system));
        if (!ION_COLLECTION_IS_EMPTY(symbols_to_append)) {
            ION_COLLECTION_OPEN(symbols_to_append, symbol_cursor);
            for (;;) {
                ION_COLLECTION_NEXT(symbol_cursor, symbol_to_append);
                if (!symbol_to_append) break;
                appended_symbol = (ION_SYMBOL *)_ion_collection_append(&cloned->symbols);
                // These strings have the same owner; they can be assigned rather than copied.
                ION_STRING_ASSIGN(&appended_symbol->value, &symbol_to_append->value);
                appended_symbol->sid = UNKNOWN_SID; // This is assigned correctly later.
            }
            ION_COLLECTION_CLOSE(symbol_cursor);
        }
        // This overwrites p_symtab's reference, which will be cleaned up when its owner is freed.
        *p_symtab = cloned;
    }

    iRETURN;
}

iERR _ion_symbol_table_load_helper(ION_READER *preader, hOWNER owner, ION_SYMBOL_TABLE *system, ION_SYMBOL_TABLE **p_psymtab)
{
    iENTER;
    ION_SYMBOL_TABLE        *symtab;
    ION_TYPE                 type;
    ION_SYMBOL_TABLE_IMPORT *import;
    ION_SYMBOL              *symbol;
    int                      version = 0;
    SID                      fld_sid, sid, max_id = 0;
    BOOL                     is_shared_table, processed_symbols = FALSE, processed_imports = FALSE;
    ION_STRING               str, name;
    ION_COLLECTION_CURSOR    import_cursor, symbol_cursor;

    ASSERT(preader   != NULL);
    ASSERT(p_psymtab != NULL);

    ION_STRING_INIT(&name);
    ION_STRING_INIT(&str);

    IONCHECK(_ion_reader_get_an_annotation_helper(preader, 0, &str));
    is_shared_table = ION_STRING_EQUALS(&ION_SYMBOL_SHARED_SYMBOL_TABLE_STRING, &str);
    if (!is_shared_table && !ION_STRING_EQUALS(&ION_SYMBOL_SYMBOL_TABLE_STRING, &str)) {
        FAILWITH(IERR_NOT_A_SYMBOL_TABLE);
    }

    // shared symbol tables don't need the system table symbols, but local tables do
    if (is_shared_table) {
        IONCHECK(_ion_symbol_table_open_helper(&symtab, owner, NULL));
        symtab->system_symbol_table = system; // we still need this reference, we just don't incorporate the symbols into the table
    }
    else {
        IONCHECK(_ion_symbol_table_open_helper(&symtab, owner, system));
    }

    owner = symtab->owner;
    ASSERT(owner != NULL);

    // now we step into the struct that has the data we actually use to fill out the table
    IONCHECK(_ion_reader_step_in_helper(preader));

    for (;;) {
        IONCHECK(_ion_reader_next_helper(preader, &type));
        if (type == tid_EOF) break;
        IONCHECK(_ion_symbol_table_get_field_sid_force(preader, &fld_sid));
        switch(fld_sid) {
        case ION_SYS_SID_NAME:     /* "name" */
            if (!is_shared_table) break; // no meaning for local tables
            if (!ION_STRING_IS_NULL(&symtab->name)) break; // FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "too many names");
            if (type == tid_STRING) {
                IONCHECK(_ion_reader_read_string_helper(preader, &str));
                if (ION_STRING_IS_NULL(&str) || str.length < 1) break; // not any name we want
                IONCHECK(ion_string_copy_to_owner(owner, &name, &str));
            }
            break;
        case ION_SYS_SID_VERSION:  /* "version" */
            if (!is_shared_table) break; // no meaning for local tables
            if (symtab->version) break; // FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "too many versions");
            if (type == tid_INT) {
                IONCHECK(_ion_reader_read_int32_helper(preader, &version));
                if (version < 1) break; // FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "version must be 1 or greater");
            }
            break;
        case ION_SYS_SID_IMPORTS:  /* "imports" */
            if (processed_imports) {
                // Since struct order is not guaranteed, duplicate imports lists could be processed in any order,
                // leading to potentially-incorrect SID mappings.
                FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "Duplicate imports declaration in symbol table.");
            }
            if (type == tid_LIST) {
                IONCHECK(_ion_symbol_table_local_load_import_list(preader, owner, &symtab->import_list));
                processed_imports = TRUE;
                // For local tables, incorporate the symbols from the import list. For shared tables, the imports list
                // is purely informational.
                if (!is_shared_table && !ION_COLLECTION_IS_EMPTY(&symtab->import_list)) {
                    ION_COLLECTION_OPEN(&symtab->import_list, import_cursor);
                    for (;;) {
                        ION_COLLECTION_NEXT(import_cursor, import);
                        if (!import) break;
                        IONCHECK(_ion_symbol_table_local_incorporate_symbols(symtab, import->shared_symbol_table, import->descriptor.max_id));
                    }
                    ION_COLLECTION_CLOSE(import_cursor);
                }
            }
            else if (!is_shared_table && type == tid_SYMBOL) {
                IONCHECK(_ion_reader_read_string_helper(preader, &str));
                if (ION_STRING_EQUALS(&ION_SYMBOL_SYMBOL_TABLE_STRING, &str)) {
                    // This LST's symbols should be appended to the previous context's symbols.
                    IONCHECK(_ion_symbol_table_append(preader, owner, system, &symtab->symbols, &symtab));
                    processed_imports = TRUE;
                }
            }
            break;
        case ION_SYS_SID_SYMBOLS:  /* "symbols" */
            if (processed_symbols) {
                // Since struct order is not guaranteed, duplicate symbols lists could be processed in any order,
                // leading to potentially-incorrect SID mappings.
                FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "Duplicate symbols declaration in symbol table.");
            }
            if (type == tid_LIST) {
                IONCHECK(_ion_symbol_table_local_load_symbol_list(preader, owner, &symtab->symbols));
                processed_symbols = TRUE;
            }
            break;
        case ION_SYS_SID_MAX_ID:   /* "max_id" */
            if (!is_shared_table) break; // no meaning for local tables
            if (max_id > 0) break; // FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "too many import lists");
            if (type == tid_INT) {
                IONCHECK(_ion_reader_read_int32_helper(preader, &max_id));
                if (max_id < 1) FAILWITHMSG(IERR_INVALID_SYMBOL_TABLE, "max_id must be 1 or greater");
            }
            break;
        default:
            // we just ignore "extra" fields
            break;
        }
    }

    IONCHECK(_ion_reader_step_out_helper(preader));

    // now adjust the symbol table owner handles (hsymtab)
    // and the sid values for any local symbols we stored but
    // didn't fully initialize
    if (!ION_COLLECTION_IS_EMPTY(&symtab->symbols)) {
        symtab->has_local_symbols = TRUE; // and now we know it has some local symbols
        sid = symtab->max_id;
        ION_COLLECTION_OPEN(&symtab->symbols, symbol_cursor);
        for (;;) {
            ION_COLLECTION_NEXT(symbol_cursor, symbol);
            if (!symbol) break;
            if (symbol->sid == UNKNOWN_SID) {
                if (sid == INT32_MAX) FAILWITH(IERR_INVALID_SYMBOL);
                sid++;
                symbol->sid = sid;
            }
        }
        ION_COLLECTION_CLOSE(symbol_cursor);
        symtab->max_id = sid;
    }

    // we grabbed these values as they went by (if they were there) now assign them
    if (is_shared_table) {
        if (version > 0) {
            symtab->version = version;
        }
        else {
            symtab->version = 1;
        }
        // we can only make the max_id shorter
        if (max_id > 0 && max_id < symtab->max_id) symtab->max_id = max_id;
        if (!ION_STRING_IS_NULL(&name)) {
            ION_STRING_ASSIGN(&symtab->name, &name);
        }
    }

    IONCHECK(_ion_symbol_table_initialize_indices_helper(symtab));

    *p_psymtab = symtab;

    iRETURN;
}

iERR ion_symbol_table_unload(hSYMTAB hsymtab, hWRITER hwriter)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;
    ION_WRITER *pwriter;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (hwriter == NULL) FAILWITH(IERR_INVALID_ARG);

    symtab  = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
    pwriter = HANDLE_TO_PTR(hwriter, ION_WRITER);

    IONCHECK(_ion_symbol_table_unload_helper(symtab, pwriter));

    iRETURN;
}

iERR _ion_symbol_table_unload_helper(ION_SYMBOL_TABLE *symtab, ION_WRITER *pwriter)
{
    iENTER;
    ION_SYMBOL_TABLE_IMPORT *import;
    ION_SYMBOL              *sym;
    SID                      annotation;
    ION_COLLECTION_CURSOR    symbol_cursor, import_cursor;
    ION_SYMBOL_TABLE_TYPE    table_type;


    ASSERT(symtab != NULL);
    ASSERT(pwriter != NULL);

    IONCHECK(_ion_symbol_table_get_type_helper(symtab, &table_type));
    switch (table_type) {
    case ist_LOCAL:
        annotation = ION_SYS_SID_SYMBOL_TABLE;
        break;
    case ist_SHARED:
    case ist_SYSTEM: // system tables are just shared tables
        annotation = ION_SYS_SID_SHARED_SYMBOL_TABLE;
        break;
    default:
        annotation = UNKNOWN_SID;
        break;
    }   

    // we annotate the struct appropriately for the table type
    // with no annotation for tables that don't have a recognizable type
    if (annotation != UNKNOWN_SID) {
        IONCHECK(_ion_writer_add_annotation_sid_helper(pwriter, annotation));
    }
    IONCHECK(_ion_writer_start_container_helper(pwriter, tid_STRUCT));

    if (!ION_STRING_IS_NULL(&symtab->name)) {
        IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_NAME));
        IONCHECK(_ion_writer_write_string_helper(pwriter, &symtab->name));
    }
    if (symtab->version > 0) {
        IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_VERSION));
        IONCHECK(_ion_writer_write_int64_helper(pwriter, symtab->version));
    }

    if (!ION_COLLECTION_IS_EMPTY(&symtab->import_list)) {
        IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_IMPORTS));
        IONCHECK(_ion_writer_start_container_helper(pwriter, tid_LIST));

        ION_COLLECTION_OPEN(&symtab->import_list, import_cursor);
        for (;;) {
            ION_COLLECTION_NEXT(import_cursor, import);
            if (!import) break;
        
            IONCHECK(_ion_writer_start_container_helper(pwriter, tid_STRUCT));
            if (!ION_STRING_IS_NULL(&import->descriptor.name)) {
                IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_NAME));
                IONCHECK(_ion_writer_write_string_helper(pwriter, &import->descriptor.name));
            }
            if (import->descriptor.version > 0) {
                IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_VERSION));
                IONCHECK(_ion_writer_write_int64_helper(pwriter, import->descriptor.version));
            }
            if (import->descriptor.max_id > ION_SYS_SYMBOL_MAX_ID_UNDEFINED) {
                IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_MAX_ID));
                IONCHECK(_ion_writer_write_int64_helper(pwriter, import->descriptor.max_id));
            }
            IONCHECK(_ion_writer_finish_container_helper(pwriter));
        }
        ION_COLLECTION_CLOSE(import_cursor);
        IONCHECK(_ion_writer_finish_container_helper(pwriter));
    }

    ION_COLLECTION_CLOSE(symbol_cursor);

    if (!ION_COLLECTION_IS_EMPTY(&symtab->symbols)) {
        // start the symbols list
        IONCHECK(_ion_writer_write_field_sid_helper(pwriter, ION_SYS_SID_SYMBOLS));
        IONCHECK(_ion_writer_start_container_helper(pwriter, tid_LIST));

        ION_COLLECTION_OPEN(&symtab->symbols, symbol_cursor);
        for (;;) {
            ION_COLLECTION_NEXT(symbol_cursor, sym);
            if (!sym) break;
            IONCHECK(_ion_writer_write_string_helper(pwriter, &sym->value));
        }
        ION_COLLECTION_CLOSE(symbol_cursor);

        IONCHECK(_ion_writer_finish_container_helper(pwriter)); // close the symbol list
    }

    IONCHECK(_ion_writer_finish_container_helper(pwriter));

    iRETURN;
}

iERR ion_symbol_table_lock(hSYMTAB hsymtab)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_lock_helper(symtab));

    iRETURN;
}

iERR _ion_symbol_table_lock_helper(ION_SYMBOL_TABLE *symtab)
{
    iENTER;
    ASSERT(symtab != NULL);
    if (symtab->is_locked) SUCCEED();

    if (symtab->max_id > 0 && !INDEX_IS_ACTIVE(symtab)) {
        IONCHECK(_ion_symbol_table_initialize_indices_helper(symtab));
    }

    symtab->is_locked = TRUE;

    iRETURN;;
}

iERR ion_symbol_table_is_locked(hSYMTAB hsymtab, BOOL *p_is_locked)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_is_locked == NULL) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_is_locked_helper(symtab, p_is_locked));

    iRETURN;
}

iERR _ion_symbol_table_is_locked_helper(ION_SYMBOL_TABLE *symtab, BOOL *p_is_locked)
{
    ASSERT(symtab != NULL);
    ASSERT(p_is_locked != NULL);

    *p_is_locked = symtab->is_locked;

    return IERR_OK;
}

iERR ion_symbol_table_get_type(hSYMTAB hsymtab, ION_SYMBOL_TABLE_TYPE *p_type)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_type == NULL)  FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_type_helper(symtab, p_type));

    iRETURN;
}

iERR _ion_symbol_table_get_type_helper(ION_SYMBOL_TABLE *symtab, ION_SYMBOL_TABLE_TYPE *p_type)
{
    ION_SYMBOL_TABLE_TYPE type = ist_EMPTY;

    ASSERT(symtab != NULL);
    ASSERT(p_type != NULL);

    if (!ION_STRING_IS_NULL(&symtab->name)) {
        // it's either system or shared
        if (symtab->version == 1 
         && ION_STRING_EQUALS(&symtab->name, &ION_SYMBOL_ION_STRING)
        ) {
            type = ist_SYSTEM;
        }
        else {
            type = ist_SHARED;
        }
    }
    else {
        type = ist_LOCAL;
    }

    *p_type = type;

    return IERR_OK;
}

iERR _ion_symbol_table_get_owner(hSYMTAB hsymtab, hOWNER *howner) {
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (howner == NULL)  FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    *howner = symtab->owner;

    iRETURN;
}

iERR ion_symbol_table_get_name(hSYMTAB hsymtab, iSTRING p_name)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_name == NULL)  FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_name_helper(symtab, p_name));
    
    iRETURN;
}

iERR _ion_symbol_table_get_name_helper(ION_SYMBOL_TABLE *symtab, ION_STRING *p_name)
{
    ASSERT(symtab != NULL);
    ASSERT(p_name != NULL);

    ION_STRING_ASSIGN(p_name, &symtab->name);
    
    return IERR_OK;
}

iERR ion_symbol_table_get_version(hSYMTAB hsymtab, int32_t *p_version)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_version == NULL)  FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_version_helper(symtab, p_version));

    iRETURN;
}

iERR _ion_symbol_table_get_system_symbol_table(hSYMTAB hsymtab, hSYMTAB *p_hsymtab_system)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_hsymtab_system == NULL)  FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    *p_hsymtab_system = symtab->system_symbol_table;

    iRETURN;
}

iERR _ion_symbol_table_get_version_helper(ION_SYMBOL_TABLE *symtab, int32_t *p_version)
{
    ASSERT(symtab != NULL);
    ASSERT(p_version != NULL);

    *p_version = symtab->version;

    return IERR_OK;
}

iERR ion_symbol_table_get_max_sid(hSYMTAB hsymtab, SID *p_max_id)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_max_id == NULL)  FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_max_sid_helper(symtab, p_max_id));

    iRETURN;
}

iERR _ion_symbol_table_get_max_sid_helper(ION_SYMBOL_TABLE *symtab, SID *p_max_id)
{
    int max_id;

    ASSERT(symtab != NULL);
    ASSERT(p_max_id != NULL);

    max_id = symtab->max_id;
    if (max_id <= 0) {
        ASSERT(symtab->system_symbol_table != NULL);
        max_id = symtab->system_symbol_table->max_id;

    }

    *p_max_id = max_id;

    return IERR_OK;
}

iERR _ion_symbol_table_get_flushed_max_sid_helper(ION_SYMBOL_TABLE *symtab, SID *p_flushed_max_id)
{
    ASSERT(symtab != NULL);
    ASSERT(p_flushed_max_id != NULL);

    *p_flushed_max_id = symtab->flushed_max_id;

    return IERR_OK;
}

iERR _ion_symbol_table_set_flushed_max_sid_helper(ION_SYMBOL_TABLE *symtab, SID flushed_max_id)
{
    ASSERT(symtab != NULL);

    symtab->flushed_max_id = flushed_max_id;

    return IERR_OK;
}

iERR ion_symbol_table_set_name(hSYMTAB hsymtab, iSTRING name)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (ION_STRING_IS_NULL(name)) FAILWITH(IERR_INVALID_ARG);
    if (name->length < 1) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_set_name_helper(symtab, name));

    iRETURN;
}

iERR _ion_symbol_table_set_name_helper(ION_SYMBOL_TABLE *symtab, ION_STRING *name)
{
    iENTER;

    ASSERT(symtab != NULL);
    ASSERT(!ION_STRING_IS_NULL(name));
    ASSERT(name->length > 0);

    if (symtab->is_locked) FAILWITH(IERR_IS_IMMUTABLE);

    IONCHECK(ion_string_copy_to_owner(symtab->owner, &symtab->name, name));    

    iRETURN;
}

iERR ion_symbol_table_set_version(hSYMTAB hsymtab, int32_t version)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (version < 0) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_set_version_helper(symtab, version));

    iRETURN;
}

iERR _ion_symbol_table_set_version_helper(ION_SYMBOL_TABLE *symtab, int32_t version)
{
    iENTER;

    ASSERT(symtab != NULL);
    ASSERT(version >= 0);

    if (symtab->is_locked) FAILWITH(IERR_IS_IMMUTABLE);

    symtab->version = version;

    iRETURN;
}

iERR ion_symbol_table_set_max_sid(hSYMTAB hsymtab, SID max_id)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (max_id < 0) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_set_max_sid_helper(symtab, max_id));

    iRETURN;
}

iERR _ion_symbol_table_set_max_sid_helper(ION_SYMBOL_TABLE *symtab, SID max_id)
{
    iENTER;

    ASSERT(symtab != NULL);
    ASSERT(max_id >= 0);

    if (symtab->is_locked) FAILWITH(IERR_IS_IMMUTABLE);

    symtab->max_id = max_id;

    iRETURN;
}

iERR ion_symbol_table_get_imports(hSYMTAB hsymtab, ION_COLLECTION **p_imports)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (p_imports == NULL) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_get_imports_helper(symtab, p_imports));

    iRETURN;
}

iERR _ion_symbol_table_get_imports_helper(ION_SYMBOL_TABLE *symtab, ION_COLLECTION **p_imports)
{
    ASSERT(symtab != NULL);
    ASSERT(p_imports != NULL);

    *p_imports = &symtab->import_list;

    return IERR_OK;
}

iERR _ion_symbol_table_get_symbols_helper(ION_SYMBOL_TABLE *symtab, ION_COLLECTION **p_symbols)
{
    ASSERT(symtab != NULL);
    ASSERT(p_symbols != NULL);

    *p_symbols = &symtab->symbols;

    return IERR_OK;
}

iERR ion_symbol_table_import_symbol_table(hSYMTAB hsymtab, hSYMTAB hsymtab_import)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab, *import;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
    if (hsymtab_import == NULL) FAILWITH(IERR_INVALID_ARG);
    import = HANDLE_TO_PTR(hsymtab_import, ION_SYMBOL_TABLE);

    if (symtab->is_locked) FAILWITH(IERR_IS_IMMUTABLE);

    IONCHECK(_ion_symbol_table_import_symbol_table_helper(symtab, import, &import->name, import->version, import->max_id));

    iRETURN;
}

iERR _ion_symbol_table_import_symbol_table_helper(ION_SYMBOL_TABLE *symtab, ION_SYMBOL_TABLE *import_symtab, ION_STRING *import_name, int32_t import_version, int32_t import_max_id)
{
    iENTER;
    ION_SYMBOL_TABLE_IMPORT *import;

    import = (ION_SYMBOL_TABLE_IMPORT *)_ion_collection_append(&symtab->import_list);
    if (!import) FAILWITH(IERR_NO_MEMORY);

    memset(import, 0, sizeof(ION_SYMBOL_TABLE_IMPORT));
    import->descriptor.max_id = import_max_id;
    import->descriptor.version = import_version;
    IONCHECK(ion_string_copy_to_owner(symtab->owner, &import->descriptor.name, import_name));
    if (import_symtab && symtab->owner != import_symtab->owner) {
        IONCHECK(_ion_symbol_table_clone_with_owner_helper(&import->shared_symbol_table, import_symtab, symtab->owner,
                                                           import_symtab->system_symbol_table));
    }
    else {
        import->shared_symbol_table = import_symtab;
    }

    IONCHECK(_ion_symbol_table_local_incorporate_symbols(symtab, import_symtab, import_max_id));

    iRETURN;
}

iERR ion_symbol_table_add_import(hSYMTAB hsymtab, ION_SYMBOL_TABLE_IMPORT_DESCRIPTOR *p_import, hCATALOG hcatalog)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab, *shared;
    ION_CATALOG      *catalog;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);
    if (hcatalog == NULL) FAILWITH(IERR_INVALID_ARG);
    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
    catalog = HANDLE_TO_PTR(hcatalog, ION_CATALOG);
    if (p_import == NULL) FAILWITH(IERR_INVALID_ARG);
    if (symtab->is_locked) FAILWITH(IERR_IS_IMMUTABLE);
    if (symtab->has_local_symbols) FAILWITH(IERR_HAS_LOCAL_SYMBOLS);

    IONCHECK(_ion_catalog_find_best_match_helper(catalog, &p_import->name, p_import->version, p_import->max_id, &shared));

    IONCHECK(_ion_symbol_table_import_symbol_table_helper(symtab, shared, &p_import->name, p_import->version, p_import->max_id));

    iRETURN;
}

iERR _ion_symbol_table_import_compare(ION_SYMBOL_TABLE_IMPORT *lhs, ION_SYMBOL_TABLE_IMPORT *rhs, BOOL *is_equal)
{
    iENTER;
    ASSERT(is_equal);
    if (lhs == NULL ^ rhs == NULL) {
        *is_equal = FALSE;
        SUCCEED();
    }
    if (lhs == NULL) {
        ASSERT(rhs == NULL);
        *is_equal = TRUE;
        SUCCEED();
    }
    if (!ION_STRING_EQUALS(&lhs->descriptor.name, &rhs->descriptor.name)) {
        *is_equal = FALSE;
        SUCCEED();
    }
    if (lhs->descriptor.version != rhs->descriptor.version || lhs->descriptor.max_id != rhs->descriptor.max_id) {
        *is_equal = FALSE;
        SUCCEED();
    }

    *is_equal = TRUE;

    iRETURN;
}

iERR _ion_symbol_table_import_compare_fn(void *lhs, void *rhs, BOOL *is_equal)
{
    iENTER;
    ION_SYMBOL_TABLE_IMPORT *lhs_import, *rhs_import;
    lhs_import = (ION_SYMBOL_TABLE_IMPORT *)lhs;
    rhs_import = (ION_SYMBOL_TABLE_IMPORT *)rhs;
    IONCHECK(_ion_symbol_table_import_compare(lhs_import, rhs_import, is_equal));
    iRETURN;
}

iERR _ion_symbol_table_local_incorporate_symbols(ION_SYMBOL_TABLE *symtab, ION_SYMBOL_TABLE *shared, int32_t import_max_id)
{
    iENTER;
    ION_SYMBOL_TABLE_TYPE type;

    ASSERT(symtab != NULL);
    ASSERT(!symtab->is_locked);
    ASSERT(!symtab->has_local_symbols);

    if (shared) {
        IONCHECK(ion_symbol_table_get_type(shared, &type));
        if (type == ist_LOCAL || type == ist_EMPTY) {
            FAILWITH(IERR_INVALID_ARG);
        }
    }
    else if (import_max_id <= ION_SYS_SYMBOL_MAX_ID_UNDEFINED) {
        FAILWITH(IERR_INVALID_SYMBOL_TABLE);
    }

    symtab->max_id += import_max_id;
    symtab->min_local_id = symtab->max_id + 1;

    iRETURN;
}

iERR _ion_symbol_table_local_find_by_name(ION_SYMBOL_TABLE *symtab, ION_STRING *name, SID *p_sid, ION_SYMBOL **p_sym)
{
    iENTER;
    ION_COLLECTION_CURSOR   symbol_cursor;
    ION_SYMBOL             *sym;
    SID                     sid;

    if(ION_STRING_IS_NULL(name)) {
        FAILWITH(IERR_NULL_VALUE);
    }
    
    ASSERT(symtab);
    ASSERT(p_sid != NULL);

    if (!INDEX_IS_ACTIVE(symtab) && symtab->max_id > DEFAULT_INDEX_BUILD_THRESHOLD) {
        IONCHECK(_ion_symbol_table_initialize_indices_helper(symtab));
    }

    if (INDEX_IS_ACTIVE(symtab)) {
        sym = _ion_symbol_table_index_find_by_name_helper(symtab, name);
    }
    else {
        // we only do this when there aren't very many symbols (see threshold above)
        ION_COLLECTION_OPEN(&symtab->symbols, symbol_cursor);
        for (;;) {
            ION_COLLECTION_NEXT(symbol_cursor, sym);
            if (!sym) break;
            if (ION_STRING_EQUALS(name, &sym->value)) {
                break;
            }
        }
        ION_COLLECTION_CLOSE(symbol_cursor);

    }

    if (sym) {
        sid = sym->sid;
    }
    else {
        sid = UNKNOWN_SID;
    }

    if (p_sid) *p_sid = sid;
    if (p_sym) *p_sym = sym;

    iRETURN;
}

iERR ion_symbol_table_find_by_name(hSYMTAB hsymtab, iSTRING name, SID *p_sid)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL)            FAILWITH(IERR_INVALID_ARG);
    if (name == NULL)               FAILWITH(IERR_INVALID_ARG);
    if (ION_STRING_IS_NULL(name))   FAILWITH(IERR_INVALID_ARG);
    if (name->length < 1)           FAILWITH(IERR_INVALID_ARG);
    if (p_sid == NULL)              FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_find_by_name_helper(symtab, name, p_sid, NULL, FALSE));

    iRETURN;
}

iERR _ion_symbol_table_parse_possible_symbol_identifier(ION_SYMBOL_TABLE *symtab, ION_STRING *name, SID *p_sid, ION_SYMBOL **p_sym, BOOL *p_is_symbol_identifier) {
    iENTER;
    SID sid = UNKNOWN_SID;
    ION_SYMBOL *sym = NULL;
    int ii, c;
    BOOL is_symbol_identifier = FALSE;

    ASSERT(p_is_symbol_identifier);
    ASSERT(p_sym);
    ASSERT(name);
    ASSERT(symtab);

    if (name->value[0] == '$' && name->length > 1) {
        sid = 0;
        for (ii=1; ii<name->length; ii++) {
            c = name->value[ii];
            if (c < '0' || c > '9') {
                sid = UNKNOWN_SID;
                goto done;
            }
            sid *= 10;
            sid += c - '0';
        }
        is_symbol_identifier = TRUE;
        if (sid == 0 || sid > symtab->max_id) {
            // SID 0 is not in any symbol table, but is available in all symbol table contexts.
            // If the requested SID is out of range for the current symtab context, an error will be raised when the
            // user retrieves the symbol token.
            _ion_symbol_table_allocate_symbol_unknown_text(symtab->owner, sid, &sym);
        }
        else if (p_sym) {
            IONCHECK(_ion_symbol_table_find_symbol_by_sid_helper(symtab, sid, &sym));
            ASSERT(sym != NULL); // This SID is within range. It MUST have a non-NULL symbol.
            if (ION_STRING_IS_NULL(&sym->value) && ION_SYMBOL_IMPORT_LOCATION_IS_NULL(sym) && sym->sid >= symtab->min_local_id) {
                // This is a local symbol with unknown text, which is equivalent to symbol zero.
                sid = 0;
            }
        }
    }
done:
    *p_is_symbol_identifier = is_symbol_identifier;
    *p_sym = sym;
    if(p_sid) *p_sid = sid;
    iRETURN;
}

iERR _ion_symbol_table_find_by_name_helper(ION_SYMBOL_TABLE *symtab, ION_STRING *name, SID *p_sid, ION_SYMBOL **p_sym,
                                           BOOL symbol_identifiers_as_sids)
{
    iENTER;
    ION_SYMBOL_TABLE        *imported;
    SID                      sid = UNKNOWN_SID;
    ION_SYMBOL_TABLE_IMPORT *imp;
    ION_COLLECTION_CURSOR    import_cursor;
    int32_t                  offset;
    ION_SYMBOL              *sym = NULL;
    BOOL is_symbol_identifier;

    ASSERT(symtab != NULL);
    ASSERT(name != NULL);
    ASSERT(!ION_STRING_IS_NULL(name));
    ASSERT(name->length >= 0);
    ASSERT(p_sid != NULL);

    if (symbol_identifiers_as_sids) {
        IONCHECK(_ion_symbol_table_parse_possible_symbol_identifier(symtab, name, &sid, &sym, &is_symbol_identifier));
        if (is_symbol_identifier) {
            goto done;
        }
    }

    // first we check the system symbol table, if there is one
   IONCHECK(_ion_symbol_table_local_find_by_name(symtab->system_symbol_table, name, &sid, &sym));

    // first we have to look in the imported tables
    if (sid == UNKNOWN_SID && !ION_COLLECTION_IS_EMPTY(&symtab->import_list)) {
        offset = symtab->system_symbol_table->max_id;

        ION_COLLECTION_OPEN(&symtab->import_list, import_cursor);
        for (;;) {
            ION_COLLECTION_NEXT(import_cursor, imp);
            if (!imp) break;
            imported = imp->shared_symbol_table;
            // If the import is not found, skip it -- its symbols have unknown text and therefore cannot be looked up by name.
            if (imported != NULL) {
                IONCHECK(_ion_symbol_table_local_find_by_name(imported, name, &sid, &sym));
                if (sid > imp->descriptor.max_id) sid = UNKNOWN_SID;
                if (sid != UNKNOWN_SID) {
                    sid += offset;
                    break;
                }
            }
            offset += imp->descriptor.max_id;
        }
        ION_COLLECTION_CLOSE(import_cursor);
    }

    // and last we look in the local table itself
    if (sid == UNKNOWN_SID) {
        IONCHECK(_ion_symbol_table_local_find_by_name(symtab, name, &sid, &sym));
    }

done:
    *p_sid = sid;
    if (p_sym) *p_sym = sym;

    iRETURN;
}

iERR _ion_symbol_table_local_find_by_sid(ION_SYMBOL_TABLE *symtab, SID sid, ION_SYMBOL **p_sym)
{
    iENTER;
    ION_SYMBOL              *sym;
    ION_COLLECTION_CURSOR    symbol_cursor;

    ASSERT(symtab != NULL);
    ASSERT(p_sym != NULL);

    if (!INDEX_IS_ACTIVE(symtab) && symtab->max_id > DEFAULT_INDEX_BUILD_THRESHOLD) {
        IONCHECK(_ion_symbol_table_initialize_indices_helper(symtab));
    }
    if (INDEX_IS_ACTIVE(symtab)) {
        sym = _ion_symbol_table_index_find_by_sid_helper(symtab, sid);
    }
    else {
        // this really needs to be better!
        ION_COLLECTION_OPEN(&symtab->symbols, symbol_cursor);
        for(;;) {
            ION_COLLECTION_NEXT(symbol_cursor, sym);
            if (!sym) break;
            if (sym->sid == sid) {
                break;
            }
        }
        ION_COLLECTION_CLOSE(symbol_cursor);
        if (!sym && sid <= symtab->max_id) {
            _ion_symbol_table_allocate_symbol_unknown_text(symtab->owner, sid, &sym);
        }

    }
    if (sym && !ION_STRING_IS_NULL(&symtab->name)) {
        // The symbol is found and this is a shared symbol table. Set the import location.
        ION_STRING_ASSIGN(&sym->import_location.name, &symtab->name);
        sym->import_location.location = sid;
    }
    *p_sym = sym;

    iRETURN;
}

iERR ion_symbol_table_find_by_sid(hSYMTAB hsymtab, SID sid, iSTRING *p_name)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL)    FAILWITH(IERR_INVALID_ARG);
    if (sid < UNKNOWN_SID)  FAILWITH(IERR_INVALID_ARG);
    if (p_name == NULL)     FAILWITH(IERR_INVALID_ARG);

    if (sid == UNKNOWN_SID) {
        *p_name = NULL;
    }
    else {
        symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
        IONCHECK(_ion_symbol_table_find_by_sid_helper(symtab, sid, p_name));
    }

    iRETURN;
}

iERR _ion_symbol_table_find_symbol_by_sid_helper(ION_SYMBOL_TABLE *symtab, SID sid, ION_SYMBOL **p_sym)
{
    iENTER;
    ION_SYMBOL              *sym = NULL;
    ION_SYMBOL_TABLE        *imported;
    ION_SYMBOL_TABLE_IMPORT *imp;
    ION_COLLECTION_CURSOR    import_cursor;
    int32_t                  offset;

    ASSERT(symtab != NULL);
    ASSERT(sid > UNKNOWN_SID);
    ASSERT(p_sym);

    if (ION_STRING_IS_NULL(&symtab->name) && sid <= symtab->system_symbol_table->max_id) {
        // Only local symbol tables implicitly import the system symbol table. Shared symbol table SIDs start at 1.
        IONCHECK(_ion_symbol_table_local_find_by_sid(symtab->system_symbol_table, sid, &sym));
    }
    else {
        if (!ION_COLLECTION_IS_EMPTY(&symtab->import_list)) {
            offset = symtab->system_symbol_table->max_id;

            ION_COLLECTION_OPEN(&symtab->import_list, import_cursor);
            for (;;) {
                ION_COLLECTION_NEXT(import_cursor, imp);
                if (!imp) break;
                if (sid - offset <= imp->descriptor.max_id) {
                    imported = imp->shared_symbol_table;
                    if (imported != NULL) {
                        IONCHECK(_ion_symbol_table_local_find_by_sid(imported, sid - offset, &sym));
                    }
                    if (sym == NULL) {
                        // The SID is in range, but either the shared symbol table is not found, or the SID refers to a
                        // NULL slot in the shared symbol table. This symbol has unknown text.
                        _ion_symbol_table_allocate_symbol_unknown_text(symtab->owner, sid, &sym);
                        ION_STRING_ASSIGN(&sym->import_location.name, &imp->descriptor.name);
                        sym->import_location.location = sid - offset;
                    }
                    ASSERT(sym);
                    break;
                }
                offset += imp->descriptor.max_id;
            }
            ION_COLLECTION_CLOSE(import_cursor);
        }
        if (sym == NULL) {
            IONCHECK(_ion_symbol_table_local_find_by_sid(symtab, sid, &sym));
        }
    }

    *p_sym = sym;
    iRETURN;
}

iERR _ion_symbol_table_find_by_sid_helper(ION_SYMBOL_TABLE *symtab, SID sid, ION_STRING **p_name)
{
    iENTER;
    ION_SYMBOL              *sym = NULL;

    ASSERT(symtab != NULL);
    ASSERT(p_name != NULL);
    ASSERT(sid > UNKNOWN_SID);

    *p_name = NULL;

    IONCHECK(_ion_symbol_table_find_symbol_by_sid_helper(symtab, sid, &sym));
    if (sym != NULL) {
        *p_name = &sym->value;
        SUCCEED();
    }

    iRETURN;
}

iERR _ion_symbol_table_get_unknown_symbol_name(ION_SYMBOL_TABLE *symtab, SID sid, ION_STRING **p_name)
{
    iENTER;
    int32_t                  len;
    ION_STRING              *str;
    char                     temp[1 + MAX_INT32_LENGTH + 1]; // '$' <int> '\0'

    ASSERT(symtab != NULL);
    ASSERT(p_name != NULL);
    ASSERT(sid > UNKNOWN_SID);

    if (sid > symtab->max_id) {
        FAILWITHMSG(IERR_INVALID_SYMBOL, "Symbol ID out of range for the current symbol table context.");
    }
    if (sid >= symtab->min_local_id) {
        // This is a local symbol with unknown text, which is equivalent to symbol zero.
        sid = 0;
    }
    // A symbol name was not found, but the SID is within range for the current symbol table context -
    // make a symbol identifier of the form $<int> to represent the name
    temp[0] = '$';
    len = (int32_t)strlen(_ion_itoa_10(sid, temp + 1, sizeof(temp)-1)) + 1; // we're writing into the 2nd byte
    str = ion_alloc_with_owner(symtab->owner, sizeof(ION_STRING));
    if (!str) FAILWITH(IERR_NO_MEMORY);
    str->length = len;
    str->value = ion_alloc_with_owner(symtab->owner, len);
    if (!str->value) FAILWITH(IERR_NO_MEMORY);
    memcpy(str->value, temp, len);
    *p_name = str;
    iRETURN;
}

/**
 * Retrieve the text for the given SID. If the text is unknown, return a symbol identifier in the form $<int>.
 */
iERR _ion_symbol_table_find_by_sid_force(ION_SYMBOL_TABLE *symtab, SID sid, ION_STRING **p_name, BOOL *p_is_symbol_identifier)
{
    iENTER;
    BOOL is_symbol_identifier = FALSE;
    ASSERT(p_name != NULL);

    IONCHECK(_ion_symbol_table_find_by_sid_helper(symtab, sid, p_name));
    if (ION_STRING_IS_NULL(*p_name)) {
        IONCHECK(_ion_symbol_table_get_unknown_symbol_name(symtab, sid, p_name));
        is_symbol_identifier = TRUE;
    }
    if (p_is_symbol_identifier) *p_is_symbol_identifier = is_symbol_identifier;
    iRETURN;
}

iERR ion_symbol_table_is_symbol_known(hSYMTAB hsymtab, SID sid, BOOL *p_is_known)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL)    FAILWITH(IERR_INVALID_ARG);
    if (sid < UNKNOWN_SID)  FAILWITH(IERR_INVALID_ARG);
    if (p_is_known == NULL) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_is_symbol_known_helper(symtab, sid, p_is_known));

    iRETURN;
}

iERR _ion_symbol_table_is_symbol_known_helper(ION_SYMBOL_TABLE *symtab, SID sid, BOOL *p_is_known)
{
    iENTER;
    ION_STRING *pname = NULL;

    ASSERT(symtab != NULL);
    ASSERT(p_is_known != NULL);
    IONCHECK(_ion_symbol_table_find_by_sid_helper(symtab, sid, &pname));

    *p_is_known = ION_STRING_IS_NULL(pname);

    iRETURN;
}

// get symbols by sid, iterate from 1 to max_sid - returns all symbol
iERR ion_symbol_table_get_symbol(hSYMTAB hsymtab, SID sid, ION_SYMBOL **p_sym)
{
    iENTER;
    ION_SYMBOL *sym;
    ION_SYMBOL_TABLE *symtab;

    if (!hsymtab) FAILWITH(IERR_INVALID_ARG);
    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
    if (sid < UNKNOWN_SID || sid > symtab->max_id) FAILWITH(IERR_INVALID_ARG);
    if (!p_sym) FAILWITH(IERR_INVALID_ARG);

    IONCHECK(_ion_symbol_table_find_symbol_by_sid_helper(symtab, sid, &sym));
    *p_sym = sym;

    iRETURN;
}

// get symbols by sid, iterate from 1 to max_sid - returns only locally defined symbols
iERR ion_symbol_table_get_local_symbol(hSYMTAB hsymtab, SID sid, ION_SYMBOL **p_sym)
{
    iENTER;
    ION_SYMBOL *sym;
    ION_SYMBOL_TABLE *symtab;

    if (!hsymtab) FAILWITH(IERR_INVALID_ARG);
    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);
    if (sid < UNKNOWN_SID || sid > symtab->max_id) FAILWITH(IERR_INVALID_ARG);
    if (!p_sym) FAILWITH(IERR_INVALID_ARG);

    IONCHECK(_ion_symbol_table_local_find_by_sid(symtab, sid, &sym));
    *p_sym = sym;

    iRETURN;
}


iERR ion_symbol_table_add_symbol(hSYMTAB hsymtab, iSTRING name, SID *p_sid)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL)            FAILWITH(IERR_INVALID_ARG);
    if (ION_STRING_IS_NULL(name))   FAILWITH(IERR_INVALID_ARG);
    if (name->length < 0)           FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_add_symbol_helper(symtab, name, p_sid));

    iRETURN;
}

iERR _ion_symbol_table_add_symbol_helper(ION_SYMBOL_TABLE *symtab, ION_STRING *name, SID *p_sid)
{
    iENTER;
    SID         sid;
    ION_SYMBOL *sym;

    ASSERT(symtab != NULL);
    ASSERT(!ION_STRING_IS_NULL(name));

    IONCHECK(_ion_symbol_table_find_by_name_helper(symtab, name, &sid, &sym, FALSE));
    if (sid == UNKNOWN_SID) {
        // make sure it's really ok to add new symbols
        if (symtab->is_locked) FAILWITH(IERR_IS_IMMUTABLE);

        // we'll assign this symbol to the next id (_add_ will update max_id for us)
        sid = symtab->max_id + 1;
        IONCHECK(_ion_symbol_table_local_add_symbol_helper(symtab, name, sid, &sym));
    }

    if (sym) sym->add_count++;
    if (p_sid) *p_sid = sid;

    iRETURN;
}

iERR _ion_symbol_table_local_add_symbol_helper(ION_SYMBOL_TABLE *symtab, ION_STRING *name, SID sid, ION_SYMBOL **p_psym)
{
    iENTER;
    ION_SYMBOL *sym;
    SIZE        trailing_bytes = 0;

    ASSERT(symtab != NULL);
    ASSERT(sid > UNKNOWN_SID);
    ASSERT(!symtab->is_locked);

    sym = (ION_SYMBOL *)_ion_collection_append(&symtab->symbols);
    if (!sym) FAILWITH(IERR_NO_MEMORY);
    memset(sym, 0, sizeof(ION_SYMBOL));

    if (!ION_STRING_IS_NULL(name)) {
        ASSERT(name->length >= 0);
        // see if the named they passed is value bytes
        IONCHECK(_ion_reader_binary_validate_utf8(name->value, name->length, trailing_bytes, &trailing_bytes));
        if (trailing_bytes != 0) FAILWITH(IERR_INVALID_UTF8);


        IONCHECK(ion_string_copy_to_owner(symtab->owner, &sym->value, name));
    }
    sym->sid = sid;
    if (sym->sid > symtab->max_id) 
    {
        symtab->max_id = sym->sid;
    }
    symtab->has_local_symbols = TRUE;

    if (INDEX_IS_ACTIVE(symtab)) {
        IONCHECK(_ion_symbol_table_index_insert_helper(symtab, sym));
    }

    if (p_psym) *p_psym = sym;

    iRETURN;
}

iERR ion_symbol_table_close(hSYMTAB hsymtab)
{
    iENTER;
    ION_SYMBOL_TABLE *symtab;

    if (hsymtab == NULL) FAILWITH(IERR_INVALID_ARG);

    symtab = HANDLE_TO_PTR(hsymtab, ION_SYMBOL_TABLE);

    IONCHECK(_ion_symbol_table_close_helper(symtab));

    iRETURN;
}

iERR _ion_symbol_table_close_helper(ION_SYMBOL_TABLE *symtab)
{
    iENTER;
    ION_SYMBOL_TABLE_TYPE table_type;

    ASSERT(symtab != NULL);

    IONCHECK(ion_symbol_table_get_type(symtab, &table_type));

    if (table_type == ist_SYSTEM) {
        FAILWITH(IERR_INVALID_ARG);
    }

    if (symtab->owner == symtab) {
        ion_free_owner(symtab);
    }

    iRETURN;
}

BOOL _ion_symbol_table_sid_is_IVM(SID sid) {
    // If more IVMs are added to support future versions of Ion, they need to be added here.
    return sid == ION_SYS_SID_IVM;
}

enum version_marker_state { START, MAJOR_VERSION, UNDERSCORE, MINOR_VERSION };

static inline int add_digit(int i, char digit) {
    return 10 * i + (digit - '0');
}

BOOL _ion_symbol_table_parse_version_marker(ION_STRING *version_marker, int *major_version, int *minor_version)
{
    const char* prefix = "$ion_";
    const size_t prefix_length = 5;
    if (version_marker->length <= prefix_length) {
        return FALSE;
    }
    if (0 != strncmp(prefix, (char *)version_marker->value, prefix_length)) {
        return FALSE;
    }

    enum version_marker_state state = START;
    char c;
    int major_version_so_far = 0;
    int minor_version_so_far = 0;

    for (size_t i = prefix_length; i < version_marker->length; i++) {
        c = version_marker->value[i];
        switch (state) {
            case START:
                if (isdigit(c)) {
                    major_version_so_far = add_digit(major_version_so_far, c);
                    state = MAJOR_VERSION;
                } else {
                    return FALSE;
                }
                break;
            case MAJOR_VERSION:
                if (c == '_') {
                    state = UNDERSCORE;
                } else if (isdigit(c)) {
                    major_version_so_far = add_digit(major_version_so_far, c);
                } else {
                    return FALSE;
                }
                break;
            case UNDERSCORE:
                if (isdigit(c)) {
                    minor_version_so_far = add_digit(minor_version_so_far, c);
                    state = MINOR_VERSION;
                } else {
                    return FALSE;
                }
                break;
            case MINOR_VERSION:
                if (isdigit(c)) {
                    minor_version_so_far = add_digit(minor_version_so_far, c);
                } else {
                    return FALSE;
                }
                break;
        }
    }

    if (state != MINOR_VERSION) {
        return FALSE;
    }

    if (major_version) *major_version = major_version_so_far;
    if (minor_version) *minor_version = minor_version_so_far;
    return TRUE;
}

//
// these are helpers for validating symbols for a variety of purposes
// especially to handle cstr vs ion_string at the user boundary
//

BOOL _ion_symbol_needs_quotes(ION_STRING *p_str, BOOL symbol_identifiers_need_quotes)
{
    char *start, *limit, *cp;
    BOOL  is_possible_keyword = FALSE;
    SIZE length;

    if (!p_str || !p_str->value) return FALSE;

    cp = (char *)p_str->value;
    length = p_str->length;

    if (length < 1) return TRUE;

    start = cp;
    limit = cp + length;

    if (*cp == '$') {
        if (symbol_identifiers_need_quotes) {
            while (++cp < limit) {
                if (*cp < '0' || *cp > '9') {
                    is_possible_keyword = FALSE;
                    break;
                }
                is_possible_keyword = TRUE;
            }
            if (is_possible_keyword) {
                // Symbol identifiers (of the form $<int>) are reserved and must be quoted if provided by a user.
                return TRUE;
            }
            cp = start;
        }
    }

    // check the first character for $, _, or alpha
    switch(*cp) {
        case '$': case '_':
            break;
        case 't': case 'f': case 'n': // true, false, null, nan
            is_possible_keyword = TRUE;
            break;
        case 'a': case 'b': case 'c': case 'd': case 'e': /* 'f' */
        case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
        case 'm': /* 'n' */ case 'o': case 'p': case 'q': case 'r':
        case 's': /* 't' */ case 'u': case 'v': case 'w': case 'x':
        case 'y': case 'z':
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
        case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
        case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
        case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
        case 'Y': case 'Z':
            break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        default:
            return TRUE;
    }
    cp++;

    // now check the rest
    while (cp < limit) {
        switch(*cp) {
                // all alpha-numeric that are non-leading chars in: false, true, nan and null
                // which is: a e l n r s u
            case 'a': case 'e': case 'l': case 'n':
            case 'r': case 's': case 'u':
                break;
                // all alpha-numeric that are NOT non-leading chars in: false, true, nan and null
            case '$': case '_':
                /* 'a' */ case 'b': case 'c': case 'd': /* 'e' */ case 'f':
            case 'g': case 'h': case 'i': case 'j': case 'k': /* 'l' */
            case 'm': /* 'n' */ case 'o': case 'p': case 'q': /* 'r' */
                /* 's' */ case 't': /* 'u' */ case 'v': case 'w': case 'x':
            case 'y': case 'z':
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
            case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
            case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
            case 'Y': case 'Z':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                is_possible_keyword = FALSE;
                break;
            default:
                return TRUE;
        }
        cp++;
    }

    // if the leading char was the start of one of our keywords
    // and we never hit a special character we can use the length
    // to check our
    if (is_possible_keyword) {
        switch (length) {
            case 3: // nan
                if (memcmp(start, "nan", 3) == 0) return TRUE;
                break;
            case 4: // true or null
                if (memcmp(start, "true", 4) == 0) return TRUE;
                if (memcmp(start, "null", 4) == 0) return TRUE;
                break;
            case 5: // false
                if (memcmp(start, "false", 5) == 0) return TRUE;
                break;
        }
    }

    return FALSE;
}


iERR _ion_symbol_local_copy_new_owner(void *context, void *dst, void *src, int32_t data_size)
{
    iENTER;
    ION_SYMBOL *symbol_dst = (ION_SYMBOL *)dst;
    ION_SYMBOL *symbol_src = (ION_SYMBOL *)src;

    if (data_size != sizeof(ION_SYMBOL)) FAILWITH(IERR_INVALID_ARG);
    ASSERT(context);

    IONCHECK(ion_symbol_copy_to_owner(context, symbol_dst, symbol_src));

    iRETURN;
}

iERR _ion_symbol_local_copy_same_owner(void *context, void *dst, void *src, int32_t data_size)
{
    iENTER;
    ION_SYMBOL *symbol_dst = (ION_SYMBOL *)dst;
    ION_SYMBOL *symbol_src = (ION_SYMBOL *)src;

    if (data_size != sizeof(ION_SYMBOL)) FAILWITH(IERR_INVALID_ARG);
    ASSERT(dst);
    ASSERT(src);

    symbol_dst->sid = symbol_src->sid;
    ION_STRING_ASSIGN(&symbol_dst->value, &symbol_src->value);
    ION_STRING_ASSIGN(&symbol_dst->import_location.name, &symbol_src->import_location.name);
    symbol_dst->import_location.location = symbol_src->import_location.location;

    iRETURN;
}

iERR _ion_symbol_table_local_import_copy_new_owner(void *context, void *dst, void *src, int32_t data_size)
{
    iENTER;
    ION_SYMBOL_TABLE_IMPORT *import_dst = (ION_SYMBOL_TABLE_IMPORT *)dst;
    ION_SYMBOL_TABLE_IMPORT *import_src = (ION_SYMBOL_TABLE_IMPORT *)src;

    if (data_size != sizeof(ION_SYMBOL_TABLE_IMPORT)) FAILWITH(IERR_INVALID_ARG);
    ASSERT(dst);
    ASSERT(src);
    ASSERT(context);

    memcpy(import_dst, import_src, data_size);
    IONCHECK(ion_string_copy_to_owner(context, &import_dst->descriptor.name, &import_src->descriptor.name));

    iRETURN;
}

iERR _ion_symbol_table_local_import_copy_same_owner(void *context, void *dst, void *src, int32_t data_size)
{
    iENTER;
    ION_SYMBOL_TABLE_IMPORT *import_dst = (ION_SYMBOL_TABLE_IMPORT *)dst;
    ION_SYMBOL_TABLE_IMPORT *import_src = (ION_SYMBOL_TABLE_IMPORT *)src;

    if (data_size != sizeof(ION_SYMBOL_TABLE_IMPORT)) FAILWITH(IERR_INVALID_ARG);
    ASSERT(dst);
    ASSERT(src);

    memcpy(import_dst, import_src, data_size);
    ION_STRING_ASSIGN(&import_dst->descriptor.name, &import_src->descriptor.name);

    iRETURN;
}

iERR _ion_symbol_table_initialize_indices_helper(ION_SYMBOL_TABLE *symtab) 
{
    iENTER;
    int32_t                initial_size;
    ION_COLLECTION_CURSOR  symbol_cursor;
    ION_SYMBOL            *sym;
    ION_INDEX_OPTIONS      index_options = {
        NULL,                           // void          *_memory_owner;
        _ion_symbol_table_compare_fn,   // II_COMPARE_FN  _compare_fn;
        _ion_symbol_table_hash_fn,      // II_HASH_FN     _hash_fn;
        NULL,                           // void          *_fn_context;
        0,                              // int32_t        _initial_size;  /* number of actual keys */
        0                               // uint8_t        _density_target_percent; /* whole percent for table size increases 200% is usual (and default) */
    };

    ASSERT(symtab->is_locked == FALSE);

    if (INDEX_IS_ACTIVE(symtab)) SUCCEED(); // it's been done before

    initial_size = symtab->max_id - symtab->min_local_id + 1;  // size is 0, id's are 1 based
    if (initial_size < DEFAULT_SYMBOL_TABLE_SIZE) initial_size = DEFAULT_SYMBOL_TABLE_SIZE;
    
    index_options._initial_size = initial_size;
    index_options._memory_owner = symtab->owner;

    IONCHECK(_ion_index_initialize(&symtab->by_name, &index_options));

    // copy is false --- this time
    IONCHECK(_ion_index_grow_array((void **)&symtab->by_id, 0, initial_size, sizeof(symtab->by_id[0]), FALSE, symtab->owner));
    symtab->by_id_max = initial_size - 1; // size is 0 based, id's are 1 based

    if (!ION_COLLECTION_IS_EMPTY(&symtab->symbols)) {
        // if we have symbols we should index them
        ION_COLLECTION_OPEN(&symtab->symbols, symbol_cursor);
        for (;;) {
            ION_COLLECTION_NEXT(symbol_cursor, sym);
            if (!sym) break;
            symtab->by_id[sym->sid - symtab->min_local_id] = sym;
            err = _ion_index_insert(&symtab->by_name, sym, sym);
            if (err == IERR_KEY_ALREADY_EXISTS) {
                // This symbol has previously been declared. That's fine; when accessed by name, the SID from the first
                // declaration (i.e. the lowest SID) will be returned. This is consistent with the spec.
                err = IERR_OK;
            }
            IONCHECK(err);
        }
        ION_COLLECTION_CLOSE(symbol_cursor);
    }

    iRETURN;
}

int_fast8_t  _ion_symbol_table_compare_fn(void *key1, void *key2, void *context)
{
    int_fast8_t cmp;
    ION_SYMBOL *sym1 = (ION_SYMBOL *)key1;
    ION_SYMBOL *sym2 = (ION_SYMBOL *)key2;

    ASSERT(sym1);
    ASSERT(sym2);
#ifdef DEBUG
    ASSERT(!ION_STRING_IS_NULL(&sym1->value));
    ASSERT(!ION_STRING_IS_NULL(&sym2->value));
#endif

    // this compare is for the purposes of the hash table only !
    if (sym1 == sym2) {
        cmp = 0;
    }
    else if ((cmp = (sym1->value.length - sym2->value.length)) != 0) {
        cmp = (cmp > 0) ? 1 : -1; // normalize the value
    }
    else {
        cmp = memcmp(sym1->value.value, sym2->value.value, sym1->value.length);
    }
    return cmp;
}

int_fast32_t _ion_symbol_table_hash_fn(void *key, void *context)
{
    ION_SYMBOL  *sym = (ION_SYMBOL *)key;
    int_fast32_t hash;
    int len;
    BYTE *cb;
    
    ASSERT(sym);
    
#if 0
    hash = hashfn_mem(sym->value.value, sym->value.length);
    return hash;
#else
    hash = 0;
    len = sym->value.length;
    cb = sym->value.value;
    
    while (len)
    {
        hash = *cb + (hash << 6) + (hash << 16) - hash;
        ++cb;
        --len;
    }
    // the previous hash function was only returning 24 bits.
    return hash & 0x00FFFFFF;
#endif
}

iERR _ion_symbol_table_index_insert_helper(ION_SYMBOL_TABLE *symtab, ION_SYMBOL *sym) 
{
    iENTER;
    int32_t new_count, old_count;
    SID adjusted_sid;

    ASSERT(symtab->is_locked == FALSE);
    ASSERT(INDEX_IS_ACTIVE(symtab));

    adjusted_sid = sym->sid - symtab->min_local_id;

    if (adjusted_sid > symtab->by_id_max) {
        // the +1 is because sid's are 1 based (so we're losing the 0th slot, and need 1 extra entry)
        old_count = (symtab->by_id_max + 1);
        new_count =  old_count * DEFAULT_SYMBOL_TABLE_SID_MULTIPLIER;
        if (new_count < DEFAULT_SYMBOL_TABLE_SIZE) new_count = DEFAULT_SYMBOL_TABLE_SIZE;
        IONCHECK(_ion_index_grow_array((void **)&symtab->by_id, old_count, new_count, sizeof(symtab->by_id[0]), TRUE, symtab->owner));
        symtab->by_id_max = new_count - 1; // adjust for 1 vs 0 based value (count is 0 based, id is 1 based)
    }
    else if (adjusted_sid < 0) {
        FAILWITHMSG(IERR_INVALID_STATE, "Cannot insert symbol into shared symbol space.");
    }
    symtab->by_id[adjusted_sid] = sym;

    if (!ION_STRING_IS_NULL(&sym->value)) { // Symbols with unknown text can't be looked up by name.
        err = _ion_index_insert(&symtab->by_name, sym, sym);
        if (err == IERR_KEY_ALREADY_EXISTS) {
            // A symbol with this text has already been defined. That is fine: when looked up by name, the lowest SID
            // will be returned (in accordance with the spec). When looked up by SID, both mappings will return the
            // correct text.
            err = IERR_OK;
        }
        IONCHECK(err);
    }

    iRETURN;
}

iERR _ion_symbol_table_index_remove_helper(ION_SYMBOL_TABLE *symtab, ION_SYMBOL *sym) 
{
    iENTER;
    ION_SYMBOL *old_sym;

    ASSERT(symtab->is_locked == FALSE);
    ASSERT(INDEX_IS_ACTIVE(symtab));

    if (sym->sid > symtab->max_id || sym->sid < symtab->min_local_id) FAILWITH(IERR_INVALID_STATE);
    if (sym->sid > symtab->by_id_max) SUCCEED(); // Nothing to do -- it never had a mapping.
    _ion_index_delete(&symtab->by_name, &sym->value, (void**)&old_sym);
    ASSERT( old_sym == sym );

    symtab->by_id[sym->sid - symtab->min_local_id] = NULL;
    SUCCEED();

    iRETURN;
}

ION_SYMBOL *_ion_symbol_table_index_find_by_name_helper(ION_SYMBOL_TABLE *symtab, ION_STRING *str)
{
//  iENTER;
    ION_SYMBOL *found_sym;
    ION_SYMBOL  key_sym;

    ASSERT(symtab);
    ASSERT(!ION_STRING_IS_NULL(str));
    ASSERT(INDEX_IS_ACTIVE(symtab));

    // dummy up a symbol with the right key
    key_sym.value.length = str->length;
    key_sym.value.value = str->value;

    found_sym = _ion_index_find(&symtab->by_name, &key_sym);

    return found_sym;
}

void _ion_symbol_table_allocate_symbol_unknown_text(hOWNER owner, SID sid, ION_SYMBOL **p_symbol)
{
    ASSERT(p_symbol);
    ASSERT(owner != NULL);

    ION_SYMBOL *symbol = _ion_alloc_with_owner(owner, sizeof(ION_SYMBOL));
    ION_STRING_INIT(&symbol->value); // NULLS the value.
    symbol->sid = sid;
    symbol->add_count++;
    ION_STRING_INIT(&symbol->import_location.name); // NULLS the value.
    symbol->import_location.location = UNKNOWN_SID;

    *p_symbol = symbol;
}

ION_SYMBOL *_ion_symbol_table_index_find_by_sid_helper(ION_SYMBOL_TABLE *symtab, SID sid)
{
    ION_SYMBOL *found_sym;

    ASSERT(symtab);
    ASSERT(INDEX_IS_ACTIVE(symtab));

    if (sid <= UNKNOWN_SID || sid > symtab->max_id || sid < symtab->min_local_id) {
        found_sym = NULL;
    }
    else if (sid - symtab->min_local_id > symtab->by_id_max) {
        _ion_symbol_table_allocate_symbol_unknown_text(symtab->owner, sid, &found_sym);
    }
    else {        
        found_sym = symtab->by_id[sid - symtab->min_local_id];
    }

    return found_sym;
}

iERR ion_symbol_copy_to_owner(hOWNER owner, ION_SYMBOL *dst, ION_SYMBOL *src)
{
    iENTER;

    ASSERT(dst);
    ASSERT(src);

    dst->sid = src->sid;
    dst->add_count = 0;
    IONCHECK(ion_string_copy_to_owner(owner, &dst->value, &src->value));
    IONCHECK(ion_string_copy_to_owner(owner, &dst->import_location.name, &src->import_location.name));
    dst->import_location.location = src->import_location.location;

    iRETURN;
}

iERR ion_symbol_is_equal(ION_SYMBOL *lhs, ION_SYMBOL *rhs, BOOL *is_equal)
{
    iENTER;

    ASSERT(is_equal);

    if (lhs == rhs) {
        // Both inputs are the same reference, or NULL.
        *is_equal = TRUE;
    }
    else if (!lhs ^ !rhs) {
        // Only one of the inputs is NULL.
        *is_equal = FALSE;
    }
    // Both are non-NULL, and are not the same reference.
    else if (ION_STRING_IS_NULL(&lhs->value) ^ ION_STRING_IS_NULL(&rhs->value)) {
        // Only one of the inputs has unknown text.
        *is_equal = FALSE;
    }
    else if (ION_STRING_IS_NULL(&lhs->value)) {
        ASSERT(ION_STRING_IS_NULL(&rhs->value));
        if (ION_SYMBOL_IMPORT_LOCATION_IS_NULL(rhs) ^ ION_SYMBOL_IMPORT_LOCATION_IS_NULL(lhs)) {
            *is_equal = FALSE;
        }
        else if (!ION_SYMBOL_IMPORT_LOCATION_IS_NULL(rhs)) {
            ASSERT(!ION_SYMBOL_IMPORT_LOCATION_IS_NULL(lhs));
            // Both are shared symbols with unknown text. They are equivalent only if their import locations are
            // equivalent.
            *is_equal = ION_STRING_EQUALS(&lhs->import_location.name, &rhs->import_location.name)
                        && (lhs->import_location.location == rhs->import_location.location);
        }
        else if (lhs->sid <= UNKNOWN_SID || rhs->sid <= UNKNOWN_SID) {
            ASSERT(ION_SYMBOL_IMPORT_LOCATION_IS_NULL(lhs));
            FAILWITH(IERR_INVALID_SYMBOL);
        }
        else {
            // All local symbols with unknown text are equivalent to each other (and to symbol zero).
            ASSERT(ION_SYMBOL_IMPORT_LOCATION_IS_NULL(lhs));
            *is_equal = TRUE;
        }
    }
    else if (ION_STRING_EQUALS(&lhs->value, &rhs->value)) {
        // Both inputs have the same text. They are equivalent regardless of SID or import location.
        *is_equal = TRUE;
    }
    else {
        *is_equal = FALSE;
    }
    iRETURN;
}

const char *ion_symbol_table_type_to_str(ION_SYMBOL_TABLE_TYPE t)
{
    switch (t) {
    case ist_EMPTY:  return "ist_EMPTY";  // 0
    case ist_LOCAL:  return "ist_LOCAL";  // 1
    case ist_SHARED: return "ist_SHARED"; // 2
    case ist_SYSTEM: return "ist_SYSTEM"; // 3
    default:
        return _ion_hack_bad_value_to_str((intptr_t)t, "Bad ION_SYMBOL_TABLE_TYPE");
    }
}
