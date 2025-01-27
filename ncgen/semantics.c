/*********************************************************************
 *   Copyright 2018, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *********************************************************************/
/* $Id: semantics.c,v 1.4 2010/05/24 19:59:58 dmh Exp $ */
/* $Header: /upc/share/CVS/netcdf-3/ncgen/semantics.c,v 1.4 2010/05/24 19:59:58 dmh Exp $ */

#include        "includes.h"
#include        "dump.h"
#include        "ncoffsets.h"
#include        "netcdf_aux.h"
#include	"ncpathmgr.h"

#define floordiv(x,y) ((x) / (y))
#define ceildiv(x,y) (((x) % (y)) == 0 ? ((x) / (y)) : (((x) / (y)) + 1))

/* Forward*/
static void filltypecodes(void);
static void processenums(void);
static void processeconstrefs(void);
static void processtypes(void);
static void processtypesizes(void);
static void processvars(void);
static void processattributes(void);
static void processunlimiteddims(void);
static void processeconstrefs(void);
static void processeconstrefsR(Symbol*,Datalist*);
static void processroot(void);
static void processvardata(void);

static void computefqns(void);
static void fixeconstref(Symbol*,NCConstant* con);
static void inferattributetype(Symbol* asym);
static void validateNIL(Symbol* sym);
static void checkconsistency(void);
static int tagvlentypes(Symbol* tsym);
static Symbol* uniquetreelocate(Symbol* refsym, Symbol* root);
static char* createfilename(void);

List* vlenconstants;  /* List<Constant*>;*/
			  /* ptr to vlen instances across all datalists*/

/* Post-parse semantic checks and actions*/
void
processsemantics(void)
{
    /* Fix up the root name to match the chosen filename */
    processroot();
    /* Fill in the fqn for every defining symbol */
    computefqns();
    /* Process each type and sort by dependency order*/
    processtypes();
    /* Make sure all typecodes are set if basetype is set*/
    filltypecodes();
    /* Process each type to compute its size*/
    processtypesizes();
    /* Process each var to fill in missing fields, etc*/
    processvars();
    /* Process attributes to connect to corresponding variable*/
    processattributes();
    /* Fix up enum constant values*/
    processenums();
    /* Fix up enum constant references*/
    processeconstrefs();
    /* Compute the unlimited dimension sizes */
    processunlimiteddims();
    /* Rebuild var datalists to show dim levels */
    processvardata();
    /* check internal consistency*/
    checkconsistency();
}

/*
Given a reference symbol, produce the corresponding
definition symbol; return NULL if there is no definition
Note that this is somewhat complicated to conform to
various scoping rules, namely:
1. look into parent hierarchy for un-prefixed dimension names.
2. look in whole group tree for un-prefixed type names;
   search is depth first. MODIFIED 5/26/2009: Search is as follows:
   a. search parent hierarchy for matching type names.
   b. search whole tree for unique matching type name
   c. complain and require prefixed name.
3. look in the same group as ref for un-prefixed variable names.
4. ditto for group references
5. look in whole group tree for un-prefixed enum constants;
   result must be unique
*/

Symbol*
locate(Symbol* refsym)
{
    Symbol* sym = NULL;
    switch (refsym->objectclass) {
    case NC_DIM:
	if(refsym->is_prefixed) {
	    /* locate exact dimension specified*/
	    sym = lookup(NC_DIM,refsym);
	} else { /* Search for matching dimension in all parent groups*/
	    Symbol* parent = lookupgroup(refsym->prefix);/*get group for refsym*/
	    while(parent != NULL) {
		/* search this parent for matching name and type*/
		sym = lookupingroup(NC_DIM,refsym->name,parent);
		if(sym != NULL) break;
		parent = parent->container;
	    }
	}
	break;
    case NC_TYPE:
	if(refsym->is_prefixed) {
	    /* locate exact type specified*/
	    sym = lookup(NC_TYPE,refsym);
	} else {
	    Symbol* parent;
	    int i; /* Search for matching type in all groups (except...)*/
	    /* Short circuit test for primitive types*/
	    for(i=NC_NAT;i<=NC_STRING;i++) {
		Symbol* prim = basetypefor(i);
		if(prim == NULL) continue;
	        if(strcmp(refsym->name,prim->name)==0) {
		    sym = prim;
		    break;
		}
	    }
	    if(sym == NULL) {
	        /* Added 5/26/09: look in parent hierarchy first */
	        parent = lookupgroup(refsym->prefix);/*get group for refsym*/
	        while(parent != NULL) {
		    /* search this parent for matching name and type*/
		    sym = lookupingroup(NC_TYPE,refsym->name,parent);
		    if(sym != NULL) break;
		    parent = parent->container;
		}
	    }
	    if(sym == NULL) {
	        sym = uniquetreelocate(refsym,rootgroup); /* want unique */
	    }
	}
	break;
    case NC_VAR:
	if(refsym->is_prefixed) {
	    /* locate exact variable specified*/
	    sym = lookup(NC_VAR,refsym);
	} else {
	    Symbol* parent = lookupgroup(refsym->prefix);/*get group for refsym*/
   	    /* search this parent for matching name and type*/
	    sym = lookupingroup(NC_VAR,refsym->name,parent);
	}
        break;
    case NC_GRP:
	if(refsym->is_prefixed) {
	    /* locate exact group specified*/
	    sym = lookup(NC_GRP,refsym);
	} else {
 	    Symbol* parent = lookupgroup(refsym->prefix);/*get group for refsym*/
   	    /* search this parent for matching name and type*/
	    sym = lookupingroup(NC_GRP,refsym->name,parent);
	}
	break;

    default: PANIC1("locate: bad refsym type: %d",refsym->objectclass);
    }
    if(debug > 1) {
	char* ncname;
	if(refsym->objectclass == NC_TYPE)
	    ncname = ncclassname(refsym->subclass);
	else
	    ncname = ncclassname(refsym->objectclass);
	fdebug("locate: %s: %s -> %s\n",
		ncname,fullname(refsym),(sym?fullname(sym):"NULL"));
    }
    return sym;
}

/*
Search for an object in all groups using preorder depth-first traversal.
Return NULL if symbol is not unique or not found at all.
*/
static Symbol*
uniquetreelocate(Symbol* refsym, Symbol* root)
{
    unsigned long i;
    Symbol* sym = NULL;
    /* search the root for matching name and major type*/
    sym = lookupingroup(refsym->objectclass,refsym->name,root);
    if(sym == NULL) {
	for(i=0;i<listlength(root->subnodes);i++) {
	    Symbol* grp = (Symbol*)listget(root->subnodes,i);
	    if(grp->objectclass == NC_GRP && !grp->ref.is_ref) {
		Symbol* nextsym = uniquetreelocate(refsym,grp);
		if(nextsym != NULL) {
		    if(sym != NULL) return NULL; /* not unique */
		    sym = nextsym;
		}
	    }
	}
    }
    return sym;
}

/*
Compute the fqn for every top-level definition symbol
*/
static void
computefqns(void)
{
    unsigned long i,j;
    /* Groups first */
    for(i=0;i<listlength(grpdefs);i++) {
        Symbol* sym = (Symbol*)listget(grpdefs,i);
	topfqn(sym);
    }
    /* Dimensions */
    for(i=0;i<listlength(dimdefs);i++) {
        Symbol* sym = (Symbol*)listget(dimdefs,i);
	topfqn(sym);
    }
    /* types */
    for(i=0;i<listlength(typdefs);i++) {
        Symbol* sym = (Symbol*)listget(typdefs,i);
	topfqn(sym);
    }
    /* variables */
    for(i=0;i<listlength(vardefs);i++) {
        Symbol* sym = (Symbol*)listget(vardefs,i);
	topfqn(sym);
    }
    /* fill in the fqn names of econsts */
    for(i=0;i<listlength(typdefs);i++) {
        Symbol* sym = (Symbol*)listget(typdefs,i);
	if(sym->subclass == NC_ENUM) {
	    for(j=0;j<listlength(sym->subnodes);j++) {
		Symbol* econ = (Symbol*)listget(sym->subnodes,j);
		nestedfqn(econ);
	    }
	}
    }
    /* fill in the fqn names of fields */
    for(i=0;i<listlength(typdefs);i++) {
        Symbol* sym = (Symbol*)listget(typdefs,i);
	if(sym->subclass == NC_COMPOUND) {
	    for(j=0;j<listlength(sym->subnodes);j++) {
		Symbol* field = (Symbol*)listget(sym->subnodes,j);
		nestedfqn(field);
	    }
	}
    }
    /* fill in the fqn names of attributes */
    for(i=0;i<listlength(gattdefs);i++) {
        Symbol* sym = (Symbol*)listget(gattdefs,i);
        attfqn(sym);
    }
    for(i=0;i<listlength(attdefs);i++) {
        Symbol* sym = (Symbol*)listget(attdefs,i);
        attfqn(sym);
    }
}

/**
Process the root group.
Currently mean:
1. Compute and store the filename
*/
static void
processroot(void)
{
    rootgroup->file.filename = createfilename();
}

/* 1. Do a topological sort of the types based on dependency*/
/*    so that the least dependent are first in the typdefs list*/
/* 2. fill in type typecodes*/
/* 3. mark types that use vlen*/
static void
processtypes(void)
{
    unsigned long i,j;
    int keep,added;
    List* sorted = listnew(); /* hold re-ordered type set*/
    /* Prime the walk by capturing the set*/
    /*     of types that are dependent on primitive types*/
    /*     e.g. uint vlen(*) or primitive types*/
    for(i=0;i<listlength(typdefs);i++) {
        Symbol* sym = (Symbol*)listget(typdefs,i);
	keep=0;
	switch (sym->subclass) {
	case NC_PRIM: /*ignore pre-defined primitive types*/
	    sym->touched=1;
	    break;
	case NC_OPAQUE:
	case NC_ENUM:
	    keep=1;
	    break;
        case NC_VLEN: /* keep if its basetype is primitive*/
	    if(sym->typ.basetype->subclass == NC_PRIM) keep=1;
	    break;
	case NC_COMPOUND: /* keep if all fields are primitive*/
	    keep=1; /*assume all fields are primitive*/
	    for(j=0;j<listlength(sym->subnodes);j++) {
		Symbol* field = (Symbol*)listget(sym->subnodes,j);
		ASSERT(field->subclass == NC_FIELD);
		if(field->typ.basetype->subclass != NC_PRIM) {keep=0;break;}
	    }
	    break;
	default: break;/* ignore*/
	}
	if(keep) {
	    sym->touched = 1;
	    listpush(sorted,(void*)sym);
	}
    }
    /* 2. repeated walk to collect level i types*/
    do {
        added=0;
        for(i=0;i<listlength(typdefs);i++) {
	    Symbol* sym = (Symbol*)listget(typdefs,i);
	    if(sym->touched) continue; /* ignore already processed types*/
	    keep=0; /* assume not addable yet.*/
	    switch (sym->subclass) {
	    case NC_PRIM:
	    case NC_OPAQUE:
	    case NC_ENUM:
		PANIC("type re-touched"); /* should never happen*/
	        break;
            case NC_VLEN: /* keep if its basetype is already processed*/
	        if(sym->typ.basetype->touched) keep=1;
	        break;
	    case NC_COMPOUND: /* keep if all fields are processed*/
	        keep=1; /*assume all fields are touched*/
	        for(j=0;j<listlength(sym->subnodes);j++) {
		    Symbol* field = (Symbol*)listget(sym->subnodes,j);
		    ASSERT(field->subclass == NC_FIELD);
		    if(!field->typ.basetype->touched) {keep=1;break;}
	        }
	        break;
	    default: break;
	    }
	    if(keep) {
		listpush(sorted,(void*)sym);
		sym->touched = 1;
		added++;
	    }
	}
    } while(added > 0);
    /* Any untouched type => circular dependency*/
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* tsym = (Symbol*)listget(typdefs,i);
	if(tsym->touched) continue;
	semerror(tsym->lineno,"Circular type dependency for type: %s",fullname(tsym));
    }
    listfree(typdefs);
    typdefs = sorted;
    /* fill in type typecodes*/
    for(i=0;i<listlength(typdefs);i++) {
        Symbol* sym = (Symbol*)listget(typdefs,i);
	if(sym->typ.basetype != NULL && sym->typ.typecode == NC_NAT)
	    sym->typ.typecode = sym->typ.basetype->typ.typecode;
    }
    /* Identify types containing vlens */
    for(i=0;i<listlength(typdefs);i++) {
        Symbol* tsym = (Symbol*)listget(typdefs,i);
	tagvlentypes(tsym);
    }
}

/* Recursively check for vlens*/
static int
tagvlentypes(Symbol* tsym)
{
    int tagged = 0;
    unsigned long j;
    switch (tsym->subclass) {
        case NC_VLEN:
	    tagged = 1;
	    tagvlentypes(tsym->typ.basetype);
	    break;
	case NC_COMPOUND: /* keep if all fields are primitive*/
	    for(j=0;j<listlength(tsym->subnodes);j++) {
		Symbol* field = (Symbol*)listget(tsym->subnodes,j);
		ASSERT(field->subclass == NC_FIELD);
		if(tagvlentypes(field->typ.basetype)) tagged = 1;
	    }
	    break;
	default: break;/* ignore*/
    }
    if(tagged) tsym->typ.hasvlen = 1;
    return tagged;
}

/* Make sure all typecodes are set if basetype is set*/
static void
filltypecodes(void)
{
    int i;
    for(i=0;i<listlength(symlist);i++) {
        Symbol* sym = listget(symlist,i);
	if(sym->typ.basetype != NULL && sym->typ.typecode == NC_NAT)
	    sym->typ.typecode = sym->typ.basetype->typ.typecode;
    }
}

static void
processenums(void)
{
    unsigned long i,j;
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* sym = (Symbol*)listget(typdefs,i);
	ASSERT(sym->objectclass == NC_TYPE);
	if(sym->subclass != NC_ENUM) continue;
	for(j=0;j<listlength(sym->subnodes);j++) {
	    Symbol* esym = (Symbol*)listget(sym->subnodes,j);
	    ASSERT(esym->subclass == NC_ECONST);
	}
    }
    /* Convert enum values to match enum type*/
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* tsym = (Symbol*)listget(typdefs,i);
	ASSERT(tsym->objectclass == NC_TYPE);
	if(tsym->subclass != NC_ENUM) continue;
	for(j=0;j<listlength(tsym->subnodes);j++) {
	    Symbol* esym = (Symbol*)listget(tsym->subnodes,j);
	    NCConstant* newec = nullconst();
	    ASSERT(esym->subclass == NC_ECONST);
	    newec->nctype = esym->typ.typecode;
	    convert1(esym->typ.econst,newec);
	    reclaimconstant(esym->typ.econst);
	    esym->typ.econst = newec;
	}
    }
}

/* Walk all data lists looking for econst refs
   and convert to point to actual definition
*/
static void
processeconstrefs(void)
{
    unsigned long i;
    /* locate all the datalist and walk them recursively */
    for(i=0;i<listlength(gattdefs);i++) {
	Symbol* att = (Symbol*)listget(gattdefs,i);
	if(att->data != NULL && listlength(att->data) > 0)
	    processeconstrefsR(att,att->data);
    }
    for(i=0;i<listlength(attdefs);i++) {
	Symbol* att = (Symbol*)listget(attdefs,i);
	if(att->data != NULL && listlength(att->data) > 0)
	    processeconstrefsR(att,att->data);
    }
    for(i=0;i<listlength(vardefs);i++) {
	Symbol* var = (Symbol*)listget(vardefs,i);
	if(var->data != NULL && listlength(var->data) > 0)
	    processeconstrefsR(var,var->data);
	if(var->var.special._Fillvalue != NULL)
	    processeconstrefsR(var,var->var.special._Fillvalue);
    }
}

/* Recursive helper for processeconstrefs */
static void
processeconstrefsR(Symbol* avsym, Datalist* data)
{
    NCConstant** dlp = NULL;
    int i;
    for(i=0,dlp=data->data;i<data->length;i++,dlp++) {
	NCConstant* con = *dlp;
	if(con->nctype == NC_COMPOUND) {
	    /* Iterate over the sublists */
	    processeconstrefsR(avsym,con->value.compoundv);
	} else if(con->nctype == NC_ECONST) {
	    fixeconstref(avsym,con);
	}
    }
}

/*
Collect all types in all groups using preorder depth-first traversal.
*/
static void
typewalk(Symbol* root, nc_type typ, List* list)
{
    unsigned long i;
    for(i=0;i<listlength(root->subnodes);i++) {
	Symbol* sym = (Symbol*)listget(root->subnodes,i);
	if(sym->objectclass == NC_GRP) {
	    typewalk(sym,typ,list);
	} else if(sym->objectclass == NC_TYPE && (typ == NC_NAT || typ == sym->subclass)) {
	    if(!listcontains(list,sym))
	        listpush(list,sym);
	}
    }
}

/* Find all user-define types of type typ in access order */
static void
orderedtypes(Symbol* avsym, nc_type typ, List* types)
{
    Symbol* container = NULL;
    listclear(types);
    /* find innermost containing group */
    if(avsym->objectclass == NC_VAR) {
	container = avsym->container;
    } else {
        ASSERT(avsym->objectclass == NC_ATT);
	container = avsym->container;
	if(container->objectclass == NC_VAR)
	    container = container->container;	    
    }	    
    /* walk up the containing groups and collect type */
    for(;container!= NULL;container = container->container) {
	int i;
	/* Walk types in the container */
	for(i=0;i<listlength(container->subnodes);i++) {
	    Symbol* sym = (Symbol*)listget(container->subnodes,i);
	    if(sym->objectclass == NC_TYPE && (typ == NC_NAT || sym->subclass == typ))
	        listpush(types,sym);
	}
    }
    /* Now do all-tree search */
    typewalk(rootgroup,typ,types);
}

static Symbol*
locateeconst(Symbol* enumt, const char* ecname)
{
    int i;
    for(i=0;i<listlength(enumt->subnodes);i++) {
        Symbol* esym = (Symbol*)listget(enumt->subnodes,i);
        ASSERT(esym->subclass == NC_ECONST);
        if(strcmp(esym->name,ecname)==0)
	    return esym;	
    }
    return NULL;
}

static Symbol*
findeconstenum(Symbol* avsym, NCConstant* con)
{
    int i;
    Symbol* refsym = con->value.enumv;
    List* typdefs = listnew();
    Symbol* enumt = NULL;
    Symbol* candidate = NULL; /* possible enum type */
    Symbol* econst = NULL;
    char* path = NULL;
    char* name = NULL;

    /* get all enum types */
    orderedtypes(avsym,NC_ENUM,typdefs);

    /* It is possible that the enum const is prefixed with the type name */

    path = strchr(refsym->name,'.');
    if(path != NULL) {
	path = strdup(refsym->name);
    	name = strchr(path,'.');
        *name++ = '\0';
    } else
        name = refsym->name;
    /* See if we can find the enum type */
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* sym = (Symbol*)listget(typdefs,i);
	ASSERT(sym->objectclass == NC_TYPE && sym->subclass == NC_ENUM);	
	if(path != NULL && strcmp(sym->name,path)==0) {enumt = sym; break;}
	/* See if enum has a matching econst */
        econst = locateeconst(sym,name);
	if(candidate == NULL && econst != NULL) candidate = sym; /* remember this */
    }
    if(enumt != NULL) goto done;
    /* otherwise use the candidate */
    enumt = candidate;
done:
    if(enumt) econst = locateeconst(enumt,name);
    listfree(typdefs);
    nullfree(path);
    if(econst == NULL)
	semerror(con->lineno,"Undefined enum constant: %s",refsym->name);
    return econst;
}

static void
fixeconstref(Symbol* avsym, NCConstant* con)
{
    Symbol* econst = NULL;

    econst = findeconstenum(avsym,con);
    assert(econst != NULL && econst->subclass == NC_ECONST);
    con->value.enumv = econst;
}

/* Compute type sizes and compound offsets*/
void
computesize(Symbol* tsym)
{
    int i;
    int offset = 0;
    int largealign;
    unsigned long totaldimsize;
    if(tsym->touched) return;
    tsym->touched=1;
    switch (tsym->subclass) {
        case NC_VLEN: /* actually two sizes for vlen*/
	    computesize(tsym->typ.basetype); /* first size*/
	    tsym->typ.size = ncsize(tsym->typ.typecode);
	    tsym->typ.alignment = ncaux_class_alignment(tsym->typ.typecode);
	    tsym->typ.nelems = 1; /* always a single compound datalist */
	    break;
	case NC_PRIM:
	    tsym->typ.size = ncsize(tsym->typ.typecode);
	    tsym->typ.alignment = ncaux_class_alignment(tsym->typ.typecode);
	    tsym->typ.nelems = 1;
	    break;
	case NC_OPAQUE:
	    /* size and alignment already assigned*/
	    tsym->typ.nelems = 1;
	    break;
	case NC_ENUM:
	    computesize(tsym->typ.basetype); /* first size*/
	    tsym->typ.size = tsym->typ.basetype->typ.size;
	    tsym->typ.alignment = tsym->typ.basetype->typ.alignment;
	    tsym->typ.nelems = 1;
	    break;
	case NC_COMPOUND: /* keep if all fields are primitive*/
	    /* First, compute recursively, the size and alignment of fields*/
            for(i=0;i<listlength(tsym->subnodes);i++) {
		Symbol* field = (Symbol*)listget(tsym->subnodes,i);
                ASSERT(field->subclass == NC_FIELD);
		computesize(field);
		if(i==0) tsym->typ.alignment = field->typ.alignment;
            }
            /* now compute the size of the compound based on what user specified*/
            offset = 0;
            largealign = 1;
            for(i=0;i<listlength(tsym->subnodes);i++) {
                Symbol* field = (Symbol*)listget(tsym->subnodes,i);
                /* only support 'c' alignment for now*/
                int alignment = field->typ.alignment;
                int padding = getpadding(offset,alignment);
                offset += padding;
                field->typ.offset = offset;
                offset += field->typ.size;
                if (alignment > largealign) {
                    largealign = alignment;
                }
            }
	    tsym->typ.cmpdalign = largealign; /* total structure size alignment */
            offset += (offset % largealign);
	    tsym->typ.size = offset;
	    break;
        case NC_FIELD: /* Compute size assume no unlimited dimensions*/
	    if(tsym->typ.dimset.ndims > 0) {
	        computesize(tsym->typ.basetype);
	        totaldimsize = crossproduct(&tsym->typ.dimset,0,rankfor(&tsym->typ.dimset));
	        tsym->typ.size = tsym->typ.basetype->typ.size * totaldimsize;
	        tsym->typ.alignment = tsym->typ.basetype->typ.alignment;
	        tsym->typ.nelems = 1;
	    } else {
	        tsym->typ.size = tsym->typ.basetype->typ.size;
	        tsym->typ.alignment = tsym->typ.basetype->typ.alignment;
	        tsym->typ.nelems = tsym->typ.basetype->typ.nelems;
	    }
	    break;
	default:
	    PANIC1("computesize: unexpected type class: %d",tsym->subclass);
	    break;
    }
}

void
processvars(void)
{
    int i,j;
    for(i=0;i<listlength(vardefs);i++) {
	Symbol* vsym = (Symbol*)listget(vardefs,i);
	Symbol* basetype = vsym->typ.basetype;
        /* If we are in classic mode, then convert long -> int32 */
	if(usingclassic) {
	    if(basetype->typ.typecode == NC_LONG || basetype->typ.typecode == NC_INT64) {
	        vsym->typ.basetype = primsymbols[NC_INT];
		basetype = vsym->typ.basetype;
	    }
        }
	/* fill in the typecode*/
	vsym->typ.typecode = basetype->typ.typecode;
	/* validate uses of NIL */
        validateNIL(vsym);
	for(j=0;j<vsym->typ.dimset.ndims;j++) {
	    /* validate the dimensions*/
            /* UNLIMITED must only be in first place if using classic */
	    if(vsym->typ.dimset.dimsyms[j]->dim.declsize == NC_UNLIMITED) {
	        if(usingclassic && j != 0)
		    semerror(vsym->lineno,"Variable: %s: UNLIMITED must be in first dimension only",fullname(vsym));
	    }
	}
    }
}

static void
processtypesizes(void)
{
    int i;
    /* use touch flag to avoid circularity*/
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* tsym = (Symbol*)listget(typdefs,i);
	tsym->touched = 0;
    }
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* tsym = (Symbol*)listget(typdefs,i);
	computesize(tsym); /* this will recurse*/
    }
}

static void
processattributes(void)
{
    int i,j;
    /* process global attributes*/
    for(i=0;i<listlength(gattdefs);i++) {
	Symbol* asym = (Symbol*)listget(gattdefs,i);
	if(asym->typ.basetype == NULL) inferattributetype(asym);
        /* fill in the typecode*/
	asym->typ.typecode = asym->typ.basetype->typ.typecode;
	if(asym->data != NULL && asym->data->length == 0) {
	    NCConstant* empty = NULL;
	    /* If the attribute has a zero length, then default it;
               note that it must be of type NC_CHAR */
	    if(asym->typ.typecode != NC_CHAR)
	        semerror(asym->lineno,"Empty datalist can only be assigned to attributes of type char",fullname(asym));
	    empty = emptystringconst(asym->lineno);
	    dlappend(asym->data,empty);
	}
	validateNIL(asym);
    }
    /* process per variable attributes*/
    for(i=0;i<listlength(attdefs);i++) {
	Symbol* asym = (Symbol*)listget(attdefs,i);
	/* If no basetype is specified, then try to infer it;
           the exception is _Fillvalue, whose type is that of the
           containing variable.
        */
        if(strcmp(asym->name,specialname(_FILLVALUE_FLAG)) == 0) {
	    /* This is _Fillvalue */
	    asym->typ.basetype = asym->att.var->typ.basetype; /* its basetype is same as its var*/
	    /* put the datalist into the specials structure */
	    if(asym->data == NULL) {
		/* Generate a default fill value */
	        asym->data = getfiller(asym->typ.basetype);
	    }
	    if(asym->att.var->var.special._Fillvalue != NULL)
	    	reclaimdatalist(asym->att.var->var.special._Fillvalue);
	    asym->att.var->var.special._Fillvalue = clonedatalist(asym->data);
	} else if(asym->typ.basetype == NULL) {
	    inferattributetype(asym);
	}
	/* fill in the typecode*/
	asym->typ.typecode = asym->typ.basetype->typ.typecode;
	if(asym->data->length == 0) {
	    NCConstant* empty = NULL;
	    /* If the attribute has a zero length, and is char type, then default it */
	    if(asym->typ.typecode != NC_CHAR)
	        semerror(asym->lineno,"Empty datalist can only be assigned to attributes of type char",fullname(asym));
	    empty = emptystringconst(asym->lineno);
	    dlappend(asym->data,empty);
	}
	validateNIL(asym);
    }
    /* collect per-variable attributes per variable*/
    for(i=0;i<listlength(vardefs);i++) {
	Symbol* vsym = (Symbol*)listget(vardefs,i);
	List* list = listnew();
        for(j=0;j<listlength(attdefs);j++) {
	    Symbol* asym = (Symbol*)listget(attdefs,j);
	    if(asym->att.var == NULL)
		continue; /* ignore globals for now */
	    if(asym->att.var != vsym) continue;
            listpush(list,(void*)asym);
	}
	vsym->var.attributes = list;
    }
}

/*
Given two types, attempt to upgrade to the "bigger type"
Rules:
- type size has precedence over signed/unsigned:
   e.g. NC_INT over NC_UBYTE
*/
static nc_type
infertype(nc_type prior, nc_type next, int hasneg)
{
    nc_type sp, sn;
    /* assert isinttype(prior) && isinttype(next) */
    if(prior == NC_NAT) return next;
    if(prior == next) return next;
    sp = signedtype(prior);
    sn = signedtype(next);
    if(sp <= sn)
	return next;
    if(sn < sp)
	return prior;
    return NC_NAT; /* all other cases illegal */
}

/*
Collect info by repeated walking of the attribute value list.
*/
static nc_type
inferattributetype1(Datalist* adata)
{
    nc_type result = NC_NAT;
    int hasneg = 0;
    int stringcount = 0;
    int charcount = 0;
    int forcefloat = 0;
    int forcedouble = 0;
    int forceuint64 = 0;
    int i;

    /* Walk the top level set of attribute values to ensure non-nesting */
    for(i=0;i<datalistlen(adata);i++) {
	NCConstant* con = datalistith(adata,i);
	if(con == NULL) return NC_NAT;
	if(con->nctype > NC_MAX_ATOMIC_TYPE) { /* illegal */
	    return NC_NAT;
	}
    }

    /* Walk repeatedly to get info for inference (loops could be combined) */
    /* Compute: all strings or chars? */
    stringcount = 0;
    charcount = 0;
    for(i=0;i<datalistlen(adata);i++) {
	NCConstant* con = datalistith(adata,i);
	if(con->nctype == NC_STRING) stringcount++;
	else if(con->nctype == NC_CHAR) charcount++;
    }
    if((stringcount+charcount) > 0) {
        if((stringcount+charcount) < datalistlen(adata))
	    return NC_NAT; /* not all textual */
	return NC_CHAR;
    }

    /* Compute: any floats/doubles? */
    forcefloat = 0;
    forcedouble = 0;
    for(i=0;i<datalistlen(adata);i++) {
	NCConstant* con = datalistith(adata,i);
	if(con->nctype == NC_FLOAT) forcefloat = 1;
	else if(con->nctype == NC_DOUBLE) {forcedouble=1; break;}
    }
    if(forcedouble) return NC_DOUBLE;
    if(forcefloat)  return NC_FLOAT;

    /* At this point all the constants should be integers */

    /* Compute: are there any uint64 values > NC_MAX_INT64? */
    forceuint64 = 0;
    for(i=0;i<datalistlen(adata);i++) {
	NCConstant* con = datalistith(adata,i);
	if(con->nctype != NC_UINT64) continue;
	if(con->value.uint64v > NC_MAX_INT64) {forceuint64=1; break;}
    }
    if(forceuint64)
	return NC_UINT64;

    /* Compute: are there any negative constants? */
    hasneg = 0;
    for(i=0;i<datalistlen(adata);i++) {
	NCConstant* con = datalistith(adata,i);
	switch (con->nctype) {
	case NC_BYTE :   if(con->value.int8v < 0)   {hasneg = 1;} break;
	case NC_SHORT:   if(con->value.int16v < 0)  {hasneg = 1;} break;
	case NC_INT:     if(con->value.int32v < 0)  {hasneg = 1;} break;
	}
    }

    /* Compute: inferred integer type */
    result = NC_NAT;
    for(i=0;i<datalistlen(adata);i++) {
	NCConstant* con = datalistith(adata,i);
	result = infertype(result,con->nctype,hasneg);
	if(result == NC_NAT) break; /* something wrong */
    }
    return result;
}

static void
inferattributetype(Symbol* asym)
{
    Datalist* datalist;
    nc_type nctype;
    ASSERT(asym->data != NULL);
    datalist = asym->data;
    if(datalist->length == 0) {
        /* Default for zero length attributes */
	asym->typ.basetype = basetypefor(NC_CHAR);
	return;
    }
    nctype = inferattributetype1(datalist);
    if(nctype == NC_NAT) { /* Illegal attribute value list */
	semerror(asym->lineno,"Non-simple list of values for untyped attribute: %s",fullname(asym));
	return;
    }
    /* get the corresponding primitive type built-in symbol*/
    /* special case for string*/
    if(nctype == NC_STRING)
        asym->typ.basetype = basetypefor(NC_CHAR);
    else if(usingclassic) {
        /* If we are in classic mode, then restrict the inferred type
           to the classic or cdf5 atypes */
	switch (nctype) {
	case NC_OPAQUE:
	case NC_ENUM:
	    nctype = NC_INT;
	    break;
	default: /* leave as is */
	    break;
	}
	asym->typ.basetype = basetypefor(nctype);
    } else
	asym->typ.basetype = basetypefor(nctype);
}

#ifdef USE_NETCDF4
/* recursive helper for validataNIL */
static void
validateNILr(Datalist* src)
{
    int i;
    for(i=0;i<src->length;i++) {
	NCConstant* con = datalistith(src,i);
	if(isnilconst(con))
            semerror(con->lineno,"NIL data can only be assigned to variables or attributes of type string");
	else if(islistconst(con)) /* recurse */
	    validateNILr(con->value.compoundv);
    }
}
#endif

static void
validateNIL(Symbol* sym)
{
#ifdef USE_NETCDF4
    Datalist* datalist = sym->data;
    if(datalist == NULL || datalist->length == 0) return;
    if(sym->typ.typecode == NC_STRING) return;
    validateNILr(datalist);
#endif
}


/* Find name within group structure*/
Symbol*
lookupgroup(List* prefix)
{
#ifdef USE_NETCDF4
    if(prefix == NULL || listlength(prefix) == 0)
	return rootgroup;
    else
	return (Symbol*)listtop(prefix);
#else
    return rootgroup;
#endif
}

/* Find name within given group*/
Symbol*
lookupingroup(nc_class objectclass, char* name, Symbol* grp)
{
    int i;
    if(name == NULL) return NULL;
    if(grp == NULL) grp = rootgroup;
dumpgroup(grp);
    for(i=0;i<listlength(grp->subnodes);i++) {
	Symbol* sym = (Symbol*)listget(grp->subnodes,i);
	if(sym->ref.is_ref) continue;
	if(sym->objectclass != objectclass) continue;
	if(strcmp(sym->name,name)!=0) continue;
	return sym;
    }
    return NULL;
}

/* Find symbol within group structure*/
Symbol*
lookup(nc_class objectclass, Symbol* pattern)
{
    Symbol* grp;
    if(pattern == NULL) return NULL;
    grp = lookupgroup(pattern->prefix);
    if(grp == NULL) return NULL;
    return lookupingroup(objectclass,pattern->name,grp);
}


/* return internal size for values of specified netCDF type */
size_t
nctypesize(
     nc_type type)			/* netCDF type code */
{
    switch (type) {
      case NC_BYTE: return sizeof(char);
      case NC_CHAR: return sizeof(char);
      case NC_SHORT: return sizeof(short);
      case NC_INT: return sizeof(int);
      case NC_FLOAT: return sizeof(float);
      case NC_DOUBLE: return sizeof(double);
      case NC_UBYTE: return sizeof(unsigned char);
      case NC_USHORT: return sizeof(unsigned short);
      case NC_UINT: return sizeof(unsigned int);
      case NC_INT64: return sizeof(long long);
      case NC_UINT64: return sizeof(unsigned long long);
      case NC_STRING: return sizeof(char*);
      default:
	PANIC("nctypesize: bad type code");
    }
    return 0;
}

static int
sqContains(List* seq, Symbol* sym)
{
    int i;
    if(seq == NULL) return 0;
    for(i=0;i<listlength(seq);i++) {
        Symbol* sub = (Symbol*)listget(seq,i);
	if(sub == sym) return 1;
    }
    return 0;
}

static void
checkconsistency(void)
{
    int i;
    for(i=0;i<listlength(grpdefs);i++) {
	Symbol* sym = (Symbol*)listget(grpdefs,i);
	if(sym == rootgroup) {
	    if(sym->container != NULL)
	        PANIC("rootgroup has a container");
	} else if(sym->container == NULL && sym != rootgroup)
	    PANIC1("symbol with no container: %s",sym->name);
	else if(sym->container->ref.is_ref != 0)
	    PANIC1("group with reference container: %s",sym->name);
	else if(sym != rootgroup && !sqContains(sym->container->subnodes,sym))
	    PANIC1("group not in container: %s",sym->name);
	if(sym->subnodes == NULL)
	    PANIC1("group with null subnodes: %s",sym->name);
    }
    for(i=0;i<listlength(typdefs);i++) {
	Symbol* sym = (Symbol*)listget(typdefs,i);
        if(!sqContains(sym->container->subnodes,sym))
	    PANIC1("type not in container: %s",sym->name);
    }
    for(i=0;i<listlength(dimdefs);i++) {
	Symbol* sym = (Symbol*)listget(dimdefs,i);
        if(!sqContains(sym->container->subnodes,sym))
	    PANIC1("dimension not in container: %s",sym->name);
    }
    for(i=0;i<listlength(vardefs);i++) {
	Symbol* sym = (Symbol*)listget(vardefs,i);
        if(!sqContains(sym->container->subnodes,sym))
	    PANIC1("variable not in container: %s",sym->name);
	if(!(isprimplus(sym->typ.typecode)
	     || sqContains(typdefs,sym->typ.basetype)))
	    PANIC1("variable with undefined type: %s",sym->name);
    }
}

static void
computeunlimitedsizes(Dimset* dimset, int dimindex, Datalist* data, int ischar)
{
    int i;
    size_t xproduct, unlimsize;
    int nextunlim,lastunlim;
    Symbol* thisunlim = dimset->dimsyms[dimindex];
    size_t length;

    ASSERT(thisunlim->dim.isunlimited);
    nextunlim = findunlimited(dimset,dimindex+1);
    lastunlim = (nextunlim == dimset->ndims);

    xproduct = crossproduct(dimset,dimindex+1,nextunlim);

    if(!lastunlim) {
	/* Compute candidate size of this unlimited */
        length = data->length;
	unlimsize = length / xproduct;
	if(length % xproduct != 0)
	    unlimsize++; /* => fill requires at some point */
#ifdef GENDEBUG2
fprintf(stderr,"unlimsize: dim=%s declsize=%lu xproduct=%lu newsize=%lu\n",
thisunlim->name,
(unsigned long)thisunlim->dim.declsize,
(unsigned long)xproduct,
(unsigned long)unlimsize);
#endif
	if(thisunlim->dim.declsize < unlimsize) /* want max length of the unlimited*/
            thisunlim->dim.declsize = unlimsize;
        /*!lastunlim => data is list of sublists, recurse on each sublist*/
	for(i=0;i<data->length;i++) {
	    NCConstant* con = data->data[i];
	    if(con->nctype != NC_COMPOUND) {
		semerror(con->lineno,"UNLIMITED dimension (other than first) must be enclosed in {}");
	    }
	    computeunlimitedsizes(dimset,nextunlim,con->value.compoundv,ischar);
	}
    } else {			/* lastunlim */
	if(ischar) {
	    /* Char case requires special computations;
	       compute total number of characters */
	    length = 0;
	    for(i=0;i<data->length;i++) {
		NCConstant* con = data->data[i];
		switch (con->nctype) {
	        case NC_CHAR: case NC_BYTE: case NC_UBYTE:
		    length++;
		    break;
		case NC_STRING:
		    length += con->value.stringv.len;
	            break;
		case NC_COMPOUND:
		    if(con->subtype == NC_DIM)
	  	        semwarn(datalistline(data),"Expected character constant, found {...}");
		    else
	     	        semwarn(datalistline(data),"Expected character constant, found (...)");
		    break;
		default:
		    semwarn(datalistline(data),"Illegal character constant: %d",con->nctype);
	        }
	    }
	} else { /* Data list should be a list of simple non-char constants */
   	    length = data->length;
	}
	unlimsize = length / xproduct;
	if(length % xproduct != 0)
	    unlimsize++; /* => fill requires at some point */
#ifdef GENDEBUG2
fprintf(stderr,"unlimsize: dim=%s declsize=%lu xproduct=%lu newsize=%lu\n",
thisunlim->name,
(unsigned long)thisunlim->dim.declsize,
(unsigned long)xproduct,
(unsigned long)unlimsize);
#endif
	if(thisunlim->dim.declsize < unlimsize) /* want max length of the unlimited*/
            thisunlim->dim.declsize = unlimsize;
    }
}

static void
processunlimiteddims(void)
{
    int i;
    /* Set all unlimited dims to size 0; */
    for(i=0;i<listlength(dimdefs);i++) {
	Symbol* dim = (Symbol*)listget(dimdefs,i);
	if(dim->dim.isunlimited)
	    dim->dim.declsize = 0;
    }
    /* Walk all variables */
    for(i=0;i<listlength(vardefs);i++) {
	Symbol* var = (Symbol*)listget(vardefs,i);
	int first,ischar;
	Dimset* dimset = &var->typ.dimset;
	if(dimset->ndims == 0) continue; /* ignore scalars */
	if(var->data == NULL) continue; /* no data list to walk */
	ischar = (var->typ.basetype->typ.typecode == NC_CHAR);
	first = findunlimited(dimset,0);
	if(first == dimset->ndims) continue; /* no unlimited dims */
	if(first == 0) {
	    computeunlimitedsizes(dimset,first,var->data,ischar);
	} else {
	    int j;
	    for(j=0;j<var->data->length;j++) {
	        NCConstant* con = var->data->data[j];
	        if(con->nctype != NC_COMPOUND)
		    semerror(con->lineno,"UNLIMITED dimension (other than first) must be enclosed in {}");
		else
	            computeunlimitedsizes(dimset,first,con->value.compoundv,ischar);
	    }
	}
    }
#ifdef GENDEBUG1
    /* print unlimited dim size */
    if(listlength(dimdefs) == 0)
        fprintf(stderr,"unlimited: no unlimited dimensions\n");
    else for(i=0;i<listlength(dimdefs);i++) {
	Symbol* dim = (Symbol*)listget(dimdefs,i);
	if(dim->dim.isunlimited)
	    fprintf(stderr,"unlimited: %s = %lu\n",
		    dim->name,
	            (unsigned long)dim->dim.declsize);
    }
#endif
}

/* Rules for specifying the dataset name:
	1. use -o name
	2. use the datasetname from the .cdl file
	3. use input cdl file name (with .cdl removed)
	It would be better if there was some way
	to specify the datasetname independently of the
	file name, but oh well.
*/
static char*
createfilename(void)
{
    char filename[4096];
    filename[0] = '\0';
    if(netcdf_name) { /* -o flag name */
      strlcat(filename,netcdf_name,sizeof(filename));
    } else { /* construct a usable output file name */
	if (cdlname != NULL && strcmp(cdlname,"-") != 0) {/* cmd line name */
	    char* p;
	    strlcat(filename,cdlname,sizeof(filename));
	    /* remove any suffix and prefix*/
	    p = strrchr(filename,'.');
	    if(p != NULL) {*p= '\0';}
	    p = strrchr(filename,'/');
	    if(p != NULL) {
		char* q = filename;
		p++; /* skip the '/' */
		while((*q++ = *p++));
	    }
       } else {/* construct name from dataset name */
	    strlcat(filename,datasetname,sizeof(filename));
        }
        /* Append the proper extension */
	strlcat(filename,binary_ext,sizeof(filename));
    }
    return strdup(filename);
}

#if 1
/* Recursive helper for processevardata */
static NCConstant*
processvardataR(Symbol* vsym, Dimset* dimset, Datalist* data, int dimindex)
{
    int rank;
    size_t offset;
    Datalist* newlist = NULL;
    NCConstant* result;
    size_t datalen;
    Symbol* dim;

    rank = dimset->ndims;
    dim = dimset->dimsyms[dimindex];

    if(rank == 0) {/* scalar */
	ASSERT((datalistlen(data) == 1));
	/* return clone of this data */
	newlist = clonedatalist(data);
	goto done;	
    }

    /* four cases to consider: (dimindex==rank-1 vs dimindex < rank-1) X (unlimited vs fixedsize)*/
    if(dimindex == (rank-1)) {/* Stop recursion here */
        if(dim->dim.isunlimited) {
  	    /* return clone of this data */
	    newlist = clonedatalist(data);
        } else { /* !unlimited */
  	    /* return clone of this data */
	    newlist = clonedatalist(data);
	}
    } else {/* dimindex < rank-1 */
        NCConstant* datacon;
	Datalist* actual;
        if(dim->dim.isunlimited && dimindex > 0) {
	    /* Should have a sequence of {..} representing the unlimited in next dimension
	       so, unbpack compound */
	    ASSERT(datalistlen(data) == 1);
	    datacon = datalistith(data,0);
	    actual = compoundfor(datacon);	    		    
	} else
	    actual = data;
        /* fall through */
        {
            newlist = builddatalist(0);
            datalen = datalistlen(actual);
            /* So we have a block of dims starting here */
   	    int nextunlim = findunlimited(dimset,dimindex+1);
            size_t dimblock;
	    int i;
	    /* compute the size of the subblocks */
            for(dimblock=1,i=dimindex+1;i<nextunlim;i++)
                dimblock *= dimset->dimsyms[i]->dim.declsize;

            /* Divide this datalist into dimblock size sublists; last may be short and process each */
            for(offset=0;;offset+=dimblock) {
                size_t blocksize;
		Datalist* subset;
    	        NCConstant* newcon;
                blocksize = (offset < datalen ? dimblock : (datalen - offset));
		subset = builddatasubset(actual,offset,blocksize);
                /* Construct a datalist to hold processed subset */
		newcon = processvardataR(vsym,dimset,subset,dimindex+1);
                dlappend(newlist,newcon);
		reclaimdatalist(subset);		
                if((offset+blocksize) >= datalen) break; /* done */
            }           
        }
    }
done:
    result = list2const(newlist);
    setsubtype(result,NC_DIM);
    return result;
}

/* listify n-dimensional data lists */
static void
processvardata(void)
{
    int i;
    for(i=0;i<listlength(vardefs);i++) {
        Symbol* vsym = (Symbol*)listget(vardefs,i);
	NCConstant* con;
        if(vsym->data == NULL) continue;
	/* Let char typed vars be handled by genchararray */
	if(vsym->typ.basetype->typ.typecode == NC_CHAR) continue;
	con = processvardataR(vsym,&vsym->typ.dimset,vsym->data,0);
	reclaimdatalist(vsym->data);
	ASSERT((islistconst(con)));
	vsym->data = compoundfor(con);
	clearconstant(con);
	freeconst(con);
    }
}

/* Convert char strings to 'x''... form */
Datalist*
explode(NCConstant* con)
{
    int i;
    char* p;
    size_t len;
    Datalist* chars;
    ASSERT((con->nctype == NC_STRING));
    len = con->value.stringv.len;
    chars = builddatalist(len);
    p = con->value.stringv.stringv;
fprintf(stderr,"p[%d]=|%s|\n",con->value.stringv.len,p);
    for(i=0;i<len;i++,p++) {
	NCConstant* chcon = nullconst();
	chcon->nctype = NC_CHAR;
	chcon->value.charv = *p;
	dlappend(chars,chcon);
    }
fprintf(stderr,"|chars|=%d\n",(int)datalistlen(chars));
    return chars;
}
#endif
