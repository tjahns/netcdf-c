/* Copyright 2018, University Corporation for Atmospheric
 * Research. See COPYRIGHT file for copying and redistribution
 * conditions. */

/**
 * @file @internal The functions which control NCZ
 * caching. These caching controls allow the user to change the cache
 * sizes of ZARR before opening files.
 *
 * @author Dennis Heimbigner, Ed Hartnett
 */

#include "zincludes.h"
#include "zcache.h"
#include "ncxcache.h"
#include "zfilter.h"

#undef DEBUG

#undef FLUSH

#define LEAFLEN 32

/* Forward */
static int get_chunk(NCZChunkCache* cache, NCZCacheEntry* entry);
static int put_chunk(NCZChunkCache* cache, NCZCacheEntry*);
static int makeroom(NCZChunkCache* cache);
static int flushcache(NCZChunkCache* cache);
static int constraincache(NCZChunkCache* cache);

/**************************************************/
/* Dispatch table per-var cache functions */

/**
 * @internal Set chunk cache size for a variable. This is the internal
 * function called by nc_set_var_chunk_cache().
 *
 * @param ncid File ID.
 * @param varid Variable ID.
 * @param size Size in bytes to set cache.
 * @param nelems # of entries in cache
 * @param preemption Controls cache swapping.
 *
 * @returns ::NC_NOERR No error.
 * @returns ::NC_EBADID Bad ncid.
 * @returns ::NC_ENOTVAR Invalid variable ID.
 * @returns ::NC_ESTRICTNC3 Attempting netcdf-4 operation on strict
 * nc3 netcdf-4 file.
 * @returns ::NC_EINVAL Invalid input.
 * @returns ::NC_EHDFERR HDF5 error.
 * @author Ed Hartnett
 */
int
NCZ_set_var_chunk_cache(int ncid, int varid, size_t cachesize, size_t nelems, float preemption)
{
    NC_GRP_INFO_T *grp;
    NC_FILE_INFO_T *h5;
    NC_VAR_INFO_T *var;
    NCZ_VAR_INFO_T *zvar;
    int retval = NC_NOERR;

    /* Check input for validity. */
    if (preemption < 0 || preemption > 1)
        {retval = NC_EINVAL; goto done;}

    /* Find info for this file and group, and set pointer to each. */
    if ((retval = nc4_find_nc_grp_h5(ncid, NULL, &grp, &h5)))
        goto done;
    assert(grp && h5);

    /* Find the var. */
    if (!(var = (NC_VAR_INFO_T *)ncindexith(grp->vars, varid)))
        {retval = NC_ENOTVAR; goto done;}
    assert(var && var->hdr.id == varid);

    zvar = (NCZ_VAR_INFO_T*)var->format_var_info;
    assert(zvar != NULL && zvar->cache != NULL);

    /* Set the values. */
    var->chunk_cache_size = cachesize;
    var->chunk_cache_nelems = nelems;
    var->chunk_cache_preemption = preemption;

    /* Fix up cache */
    if((retval = NCZ_adjust_var_cache(var))) goto done;
done:
    return retval;
}

/**
 * @internal Adjust the chunk cache of a var for better
 * performance.
 *
 * @note For contiguous and compact storage vars, or when parallel I/O
 * is in use, this function will do nothing and return ::NC_NOERR;
 *
 * @param grp Pointer to group info struct.
 * @param var Pointer to var info struct.
 *
 * @return ::NC_NOERR No error.
 * @author Ed Hartnett
 */
int
NCZ_adjust_var_cache(NC_VAR_INFO_T *var)
{
    int stat = NC_NOERR;
    NCZ_VAR_INFO_T* zvar = (NCZ_VAR_INFO_T*)var->format_var_info;
    /* completely empty the cache */
    flushcache(zvar->cache);

#ifdef DEBUG
fprintf(stderr,"xxx: adjusting cache for: %s\n",var->hdr.name);
#endif

    /* Reset the parameters */
    zvar->cache->maxsize = var->chunk_cache_size;
    zvar->cache->maxentries = var->chunk_cache_nelems;
#ifdef DEBUG
    fprintf(stderr,"%s.cache.adjust: size=%ld nelems=%ld\n",
        var->hdr.name,(unsigned long)zvar->cache->maxsize,(unsigned long)zvar->cache->maxentries);
#endif
    /* One more thing, adjust the chunksize */
    zvar->cache->chunksize = zvar->chunksize;
    /* and also rebuild the fillchunk */
    nullfree(zvar->cache->fillchunk);
    zvar->cache->fillchunk = NULL;
    if(var->no_fill)
        stat = NCZ_create_fill_chunk(zvar->cache->chunksize,var->type_info->size,NULL,&zvar->cache->fillchunk);
    else {
	assert(var->fill_value != NULL);
        stat = NCZ_create_fill_chunk(zvar->cache->chunksize,var->type_info->size,var->fill_value,&zvar->cache->fillchunk);
    }
    return stat;
}

/**************************************************/

/**
 * Create a chunk cache object
 *
 * @param var containing var
 * @param entrysize Size in bytes of an entry
 * @param cachep return cache pointer
 *
 * @return ::NC_NOERR No error.
 * @return ::NC_EINVAL Bad preemption.
 * @author Dennis Heimbigner, Ed Hartnett
 */
int
NCZ_create_chunk_cache(NC_VAR_INFO_T* var, size64_t chunksize, char dimsep, NCZChunkCache** cachep)
{
    int stat = NC_NOERR;
    NCZChunkCache* cache = NULL;
    void* fill = NULL;
    NCZ_VAR_INFO_T* zvar = NULL;
	
    if(chunksize == 0) return NC_EINVAL;

    zvar = (NCZ_VAR_INFO_T*)var->format_var_info;
    
    if((cache = calloc(1,sizeof(NCZChunkCache))) == NULL)
	{stat = NC_ENOMEM; goto done;}
    cache->var = var;
    cache->ndims = var->ndims + zvar->scalar;
    assert(cache->fillchunk == NULL);
    cache->fillchunk = NULL;
    cache->chunksize = chunksize;
    cache->dimension_separator = dimsep;
    zvar->cache = cache;

#ifdef FLUSH
    cache->maxentries = 1;
#endif

#ifdef DEBUG
    fprintf(stderr,"%s.cache: nelems=%ld size=%ld\n",
        var->hdr.name,(unsigned long)cache->maxentries,(unsigned long)cache->maxsize);
#endif
    if((stat = ncxcachenew(LEAFLEN,&cache->xcache))) goto done;
    if((cache->mru = nclistnew()) == NULL)
	{stat = NC_ENOMEM; goto done;}
    nclistsetalloc(cache->mru,cache->maxentries);
    if(cachep) {*cachep = cache; cache = NULL;}
done:
    nullfree(fill);
    NCZ_free_chunk_cache(cache);
    return THROW(stat);
}

static void
free_cache_entry(NCZCacheEntry* entry)
{
    if(entry) {
	nullfree(entry->data);
	nullfree(entry->key.varkey);
	nullfree(entry->key.chunkkey);
	nullfree(entry);
    }
}

void
NCZ_free_chunk_cache(NCZChunkCache* cache)
{
    if(cache == NULL) return;

    ZTRACE(4,"cache.var=%s",cache->var->hdr.name);

    /* Iterate over the entries */
    while(nclistlength(cache->mru) > 0) {
	void* ptr;
        NCZCacheEntry* entry = nclistremove(cache->mru,0);
	(void)ncxcacheremove(cache->xcache,entry->hashkey,&ptr);
	assert(ptr == entry);
	free_cache_entry(entry);
    }
#ifdef DEBUG
fprintf(stderr,"|cache.free|=%ld\n",nclistlength(cache->mru));
#endif
    ncxcachefree(cache->xcache);
    nclistfree(cache->mru);
    cache->mru = NULL;
    nullfree(cache->fillchunk);
    nullfree(cache);
    (void)ZUNTRACE(NC_NOERR);
}

size64_t
NCZ_cache_entrysize(NCZChunkCache* cache)
{
    assert(cache);
    return cache->chunksize;
}

/* Return number of active entries in cache */
size64_t
NCZ_cache_size(NCZChunkCache* cache)
{
    assert(cache);
    return nclistlength(cache->mru);
}

int
NCZ_read_cache_chunk(NCZChunkCache* cache, const size64_t* indices, void** datap)
{
    int stat = NC_NOERR;
    int rank = cache->ndims;
    NCZCacheEntry* entry = NULL;
    ncexhashkey_t hkey = 0;
    int created = 0;

    /* the hash key */
    hkey = ncxcachekey(indices,sizeof(size64_t)*cache->ndims);
    /* See if already in cache */
    stat = ncxcachelookup(cache->xcache,hkey,(void**)&entry);
    switch(stat) {
    case NC_NOERR:
        /* Move to front of the lru */
        (void)ncxcachetouch(cache->xcache,hkey);
        break;
    case NC_ENOOBJECT:
        entry = NULL; /* not found; */
	break;
    default: goto done;
    }

    if(entry == NULL) { /*!found*/
	/* Create a new entry */
	if((entry = calloc(1,sizeof(NCZCacheEntry)))==NULL)
	    {stat = NC_ENOMEM; goto done;}
	memcpy(entry->indices,indices,rank*sizeof(size64_t));
        /* Create the key for this cache */
        if((stat = NCZ_buildchunkpath(cache,indices,&entry->key))) goto done;
        entry->hashkey = hkey;
	/* Try to read the object from "disk" */
	if((stat=get_chunk(cache,entry))) goto done;
        nclistpush(cache->mru,entry);
	cache->used += entry->size;
	if((stat = ncxcacheinsert(cache->xcache,entry->hashkey,entry))) goto done;
	/* Ensure cache constraints not violated */
	if((stat=makeroom(cache))) goto done;
    }

#ifdef DEBUG
fprintf(stderr,"|cache.read.lru|=%ld\n",nclistlength(cache->mru));
#endif
    if(datap) *datap = entry->data;
    entry = NULL;
    
done:
    if(created && stat == NC_NOERR)  stat = NC_EEMPTY; /* tell upper layers */
    if(entry) free_cache_entry(entry);
    return THROW(stat);
}

#if 0
int
NCZ_write_cache_chunk(NCZChunkCache* cache, const size64_t* indices, void* content)
{
    int stat = NC_NOERR;
    int rank = cache->ndims;
    NCZCacheEntry* entry = NULL;
    ncexhashkey_t hkey;
    
    /* create the hash key */
    hkey = ncxcachekey(indices,sizeof(size64_t)*cache->ndims);

    if(entry == NULL) { /*!found*/
	/* Create a new entry */
	if((entry = calloc(1,sizeof(NCZCacheEntry)))==NULL)
	    {stat = NC_ENOMEM; goto done;}
	memcpy(entry->indices,indices,rank*sizeof(size64_t));
        if((stat = NCZ_buildchunkpath(cache,indices,&entry->key))) goto done;
        entry->hashkey = hkey;
	/* Create the local copy space */
	entry->size = cache->chunksize;
	if((entry->data = calloc(1,cache->chunksize)) == NULL)
	    {stat = NC_ENOMEM; goto done;}
	memcpy(entry->data,content,cache->chunksize);
    }
    entry->modified = 1;
    nclistpush(cache->mru,entry); /* MRU order */
#ifdef DEBUG
fprintf(stderr,"|cache.write|=%ld\n",nclistlength(cache->mru));
#endif
    entry = NULL;

    /* Ensure cache constraints not violated */
    if((stat=makeroom(cache))) goto done;

done:
    if(entry) free_cache_entry(entry);
    return THROW(stat);
}
#endif

/* Constrain cache, but allow at least one entry */
static int
makeroom(NCZChunkCache* cache)
{
    int stat = NC_NOERR;

    /* Sanity check; make sure at least one entry is always allowed */
    if(nclistlength(cache->mru) == 1)
	goto done;
    stat = constraincache(cache);
done:
    return stat;
}

/* Completely flush cache */

static int
flushcache(NCZChunkCache* cache)
{
    cache->maxentries = 0;
    return constraincache(cache);
}


/* Remove entries to ensure cache is not
   violating any of its constraints.
   On entry, constraints might be violated.
*/

static int
constraincache(NCZChunkCache* cache)
{
    int stat = NC_NOERR;

    /* Flush from LRU end if we are at capacity */
    while(nclistlength(cache->mru) > cache->maxentries || cache->used > cache->maxsize) {
	int i;
	void* ptr;
	NCZCacheEntry* e = ncxcachelast(cache->xcache); /* last entry is the least recently used */
        if((stat = ncxcacheremove(cache->xcache,e->hashkey,&ptr))) goto done;
	assert(e == ptr);
        for(i=0;i<nclistlength(cache->mru);i++) {
	    e = nclistget(cache->mru,i);
	    if(ptr == e) break;
	}
	assert(e != NULL);
	assert(i >= 0 && i < nclistlength(cache->mru));
	nclistremove(cache->mru,i);
	if(e->modified) /* flush to file */
	    stat=put_chunk(cache,e);
	/* Decrement space used */
	assert(cache->used >= e->size);
	cache->used -= e->size;
	/* reclaim */
        nullfree(e->data); nullfree(e->key.varkey); nullfree(e->key.chunkkey); nullfree(e);
    }
#ifdef DEBUG
fprintf(stderr,"|cache.makeroom|=%ld\n",nclistlength(cache->mru));
#endif
done:
    return stat;
}

int
NCZ_flush_chunk_cache(NCZChunkCache* cache)
{
    int stat = NC_NOERR;
    size_t i;

    ZTRACE(4,"cache.var=%s |cache|=%d",cache->var->hdr.name,(int)nclistlength(cache->mru));

    if(NCZ_cache_size(cache) == 0) goto done;
    
    /* Iterate over the entries in hashmap */
    for(i=0;i<nclistlength(cache->mru);i++) {
        NCZCacheEntry* entry = nclistget(cache->mru,i);
        if(entry->modified) {
	    /* Write out this chunk in toto*/
  	    if((stat=put_chunk(cache,entry)))
	        goto done;
	}
        entry->modified = 0;
    }

done:
    return ZUNTRACE(stat);
}

#if 0
int
NCZ_chunk_cache_modified(NCZChunkCache* cache, const size64_t* indices)
{
    int stat = NC_NOERR;
    char* key = NULL;
    NCZCacheEntry* entry = NULL;
    int rank = cache->ndims;

    /* Create the key for this cache */
    if((stat=NCZ_buildchunkkey(rank, indices, &key))) goto done;

    /* See if already in cache */
    if(NC_hashmapget(cache->mru, key, strlen(key), (uintptr_t*)entry)) { /* found */
	entry->modified = 1;
    }

done:
    nullfree(key);
    return THROW(stat);
}
#endif

/**************************************************/
/*
From Zarr V2 Specification:
"The compressed sequence of bytes for each chunk is stored under
a key formed from the index of the chunk within the grid of
chunks representing the array.  To form a string key for a
chunk, the indices are converted to strings and concatenated
with the dimension_separator character ('.' or '/') separating
each index. For example, given an array with shape (10000,
10000) and chunk shape (1000, 1000) there will be 100 chunks
laid out in a 10 by 10 grid. The chunk with indices (0, 0)
provides data for rows 0-1000 and columns 0-1000 and is stored
under the key "0.0"; the chunk with indices (2, 4) provides data
for rows 2000-3000 and columns 4000-5000 and is stored under the
key "2.4"; etc."
*/

/**
 * @param R Rank
 * @param chunkindices The chunk indices
 * @param dimsep the dimension separator
 * @param keyp Return the chunk key string
 */
int
NCZ_buildchunkkey(size_t R, const size64_t* chunkindices, char dimsep, char** keyp)
{
    int stat = NC_NOERR;
    int r;
    NCbytes* key = ncbytesnew();

    if(keyp) *keyp = NULL;

    assert(islegaldimsep(dimsep));
    
    for(r=0;r<R;r++) {
	char sindex[64];
        if(r > 0) ncbytesappend(key,dimsep);
	/* Print as decimal with no leading zeros */
	snprintf(sindex,sizeof(sindex),"%lu",(unsigned long)chunkindices[r]);	
	ncbytescat(key,sindex);
    }
    ncbytesnull(key);
    if(keyp) *keyp = ncbytesextract(key);

    ncbytesfree(key);
    return THROW(stat);
}

/**
 * @internal Push data to chunk of a file.
 * If chunk does not exist, create it
 *
 * @param file Pointer to file info struct.
 * @param proj Chunk projection
 * @param datalen size of data
 * @param data Buffer containing the chunk data to write
 *
 * @return ::NC_NOERR No error.
 * @author Dennis Heimbigner
 */
static int
put_chunk(NCZChunkCache* cache, NCZCacheEntry* entry)
{
    int stat = NC_NOERR;
    NC_FILE_INFO_T* file = NULL;
    NCZ_FILE_INFO_T* zfile = NULL;
    NCZMAP* map = NULL;
    char* path = NULL;

    ZTRACE(5,"cache.var=%s entry.key=%s",cache->var->hdr.name,entry->key);
    LOG((3, "%s: var: %p", __func__, cache->var));

    file = (cache->var->container)->nc4_info;
    zfile = file->format_file_info;
    map = zfile->map;

#ifdef ENABLE_NCZARR_FILTERS
    /* Make sure the entry is in filtered state */
    if(!entry->isfiltered) {
        NC_VAR_INFO_T* var = cache->var;
        void* filtered = NULL; /* pointer to the filtered data */
	size_t flen; /* length of filtered data */
	/* Get the filter chain to apply */
	NClist* filterchain = (NClist*)var->filters;
	if(nclistlength(filterchain) > 0) {
	    /* Apply the filter chain to get the filtered data */
	    if((stat = NCZ_applyfilterchain(file,var,filterchain,entry->size,entry->data,&flen,&filtered,ENCODING))) goto done;
	    /* Fix up the cache entry */
	    /* Note that if filtered is different from entry->data, then entry->data will have been freed */
	    entry->data = filtered;
 	    entry->size = flen;
            entry->isfiltered = 1;
	}
    }
#endif

    path = NCZ_chunkpath(entry->key);
    stat = nczmap_write(map,path,0,entry->size,entry->data);
    nullfree(path); path = NULL;

    switch(stat) {
    case NC_NOERR:
	break;
    case NC_EEMPTY:
    default: goto done;
    }
done:
    nullfree(path);
    return ZUNTRACE(stat);
}

/**
 * @internal Push data from memory to file.
 *
 * @param cache Pointer to parent cache
 * @param key chunk key
 * @param entry cache entry to read into
 *
 * @return ::NC_NOERR No error.
 * @author Dennis Heimbigner
 */
static int
get_chunk(NCZChunkCache* cache, NCZCacheEntry* entry)
{
    int stat = NC_NOERR;
    NCZMAP* map = NULL;
    NC_FILE_INFO_T* file = NULL;
    NCZ_FILE_INFO_T* zfile = NULL;
    size64_t size;
    int empty = 0;
    char* path = NULL;

    ZTRACE(5,"cache.var=%s entry.key=%s sep=%d",cache->var->hdr.name,entry->key,cache->dimension_separator);
    
    LOG((3, "%s: file: %p", __func__, file));

    file = (cache->var->container)->nc4_info;
    zfile = file->format_file_info;
    map = zfile->map;
    assert(map);

    /* get size of the "raw" data on "disk" */
    path = NCZ_chunkpath(entry->key);
    stat = nczmap_len(map,path,&size);
    nullfree(path); path = NULL;
    switch(stat) {
    case NC_NOERR: break;
    case NC_EEMPTY: empty = 1; stat = NC_NOERR; break;
    default: goto done;
    }

    if(!empty) {
        /* Make sure we have a place to read it */
        entry->size = size;
        entry->isfiltered = FILTERED(cache); /* Is the data being read filtered? */
        if((entry->data = (void*)malloc(entry->size)) == NULL)
            {stat = NC_ENOMEM; goto done;}
	/* Read the raw data */
        path = NCZ_chunkpath(entry->key);
        stat = nczmap_read(map,path,0,entry->size,(char*)entry->data);
        nullfree(path); path = NULL;
        switch (stat) {
        case NC_NOERR: break;
        case NC_EEMPTY: empty = 1; stat = NC_NOERR;break;
	default: goto done;
	}
    }
    if(empty) {
	/* fake the chunk */
        entry->modified = (file->no_write?0:1);
	entry->size = cache->chunksize;
        if((entry->data = (void*)malloc(entry->size)) == NULL)
            {stat = NC_ENOMEM; goto done;}
        /* apply fill value */
	assert(cache->fillchunk);
	memcpy(entry->data,cache->fillchunk,entry->size);
        entry->isfiltered = 0;
	stat = NC_NOERR;
    }
#ifdef ENABLE_NCZARR_FILTERS
    /* Make sure the entry is in unfiltered state */
    if(entry->isfiltered) {
        NC_VAR_INFO_T* var = cache->var;
        void* unfiltered = NULL; /* pointer to the unfiltered data */
        void* filtered = NULL; /* pointer to the filtered data */
	size_t unflen; /* length of unfiltered data */
	/* Get the filter chain to apply */
	NClist* filterchain = (NClist*)var->filters;
	if(nclistlength(filterchain) == 0) {stat = NC_EFILTER; goto done;}
	/* Apply the filter chain to get the unfiltered data */
	filtered = entry->data;
	entry->data = NULL;
	if((stat = NCZ_applyfilterchain(file,var,filterchain,entry->size,filtered,&unflen,&unfiltered,!ENCODING))) goto done;
	/* Fix up the cache entry */
	entry->data = unfiltered;
	entry->size = unflen;
	entry->isfiltered = 0;
    }
#endif

done:
    nullfree(path);
    return ZUNTRACE(stat);
}

int
NCZ_buildchunkpath(NCZChunkCache* cache, const size64_t* chunkindices, struct ChunkKey* key)
{
    int stat = NC_NOERR;
    char* chunkname = NULL;
    char* varkey = NULL;

    assert(key != NULL);
    /* Get the chunk object name */
    if((stat = NCZ_buildchunkkey(cache->ndims, chunkindices, cache->dimension_separator, &chunkname))) goto done;
    /* Get the var object key */
    if((stat = NCZ_varkey(cache->var,&varkey))) goto done;
    key->varkey = varkey; varkey = NULL;
    key->chunkkey = chunkname; chunkname = NULL;    

done:
    nullfree(chunkname);
    nullfree(varkey);
    return THROW(stat);
}
