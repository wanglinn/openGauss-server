/* -------------------------------------------------------------------------
 *
 * alter.cpp
 *	  Drivers for generic alter commands
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/commands/alter.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/tableam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_synonym.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/directory.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/subscriptioncmds.h"
#include "commands/sec_rls_cmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/syscache.h"

/*
 * Executes an ALTER OBJECT / RENAME TO statement.	Based on the object
 * type, the function appropriate to that type is executed.
 */
void ExecRenameStmt(RenameStmt* stmt)
{
    switch (stmt->renameType) {
        case OBJECT_AGGREGATE:
            /* Given ordered set aggregate with no direct args, aggr_args variable is modified in gram.y.
               So the parse of aggr_args should be changed. See gram.y for detail. */
            stmt->objarg = (List*)linitial(stmt->objarg);
            RenameAggregate(stmt->object, stmt->objarg, stmt->newname);
            break;

        case OBJECT_COLLATION:
            RenameCollation(stmt->object, stmt->newname);
            break;

        case OBJECT_CONSTRAINT:
            RenameConstraint(stmt);
            break;

        case OBJECT_CONVERSION:
            RenameConversion(stmt->object, stmt->newname);
            break;

        case OBJECT_DATABASE:
            RenameDatabase(stmt->subname, stmt->newname);
            break;

        case OBJECT_FDW:
            RenameForeignDataWrapper(stmt->subname, stmt->newname);
            break;

        case OBJECT_FOREIGN_SERVER:
            RenameForeignServer(stmt->subname, stmt->newname);
            break;

        case OBJECT_FUNCTION:
            RenameFunction(stmt->object, stmt->objarg, stmt->newname);
            break;

        case OBJECT_LANGUAGE:
            RenameLanguage(stmt->subname, stmt->newname);
            break;

        case OBJECT_OPCLASS:
            RenameOpClass(stmt->object, stmt->subname, stmt->newname);
            break;

        case OBJECT_OPFAMILY:
            RenameOpFamily(stmt->object, stmt->subname, stmt->newname);
            break;

        case OBJECT_PARTITION:
            renamePartition(stmt);
            break;

        case OBJECT_PARTITION_INDEX:
            renamePartitionIndex(stmt);
            break;
        
        case OBJECT_PUBLICATION:
            RenamePublication(stmt->object, stmt->newname);
            break;

        case OBJECT_SUBSCRIPTION:
            RenameSubscription(stmt->object, stmt->newname);
            break;

        case OBJECT_RLSPOLICY:
            RenameRlsPolicy(stmt);
            break;

        case OBJECT_ROLE:
            RenameRole(stmt->subname, stmt->newname);
            break;

        case OBJECT_USER:
            RenameRole(stmt->subname, stmt->newname);

            /*
             * Rename user need rename the schema that has the same name which
             * owned by the user.
             */
            RenameSchema(stmt->subname, stmt->newname);
            break;

        case OBJECT_SCHEMA:
            RenameSchema(stmt->subname, stmt->newname);
            break;

        case OBJECT_TABLESPACE:
            RenameTableSpace(stmt->subname, stmt->newname);
            break;

        case OBJECT_TABLE:
        case OBJECT_SEQUENCE:
        case OBJECT_LARGE_SEQUENCE:
        case OBJECT_VIEW:
        case OBJECT_CONTQUERY:
        case OBJECT_MATVIEW:
        case OBJECT_INDEX:
        case OBJECT_FOREIGN_TABLE:
        case OBJECT_STREAM:
            RenameRelation(stmt);
            break;

        case OBJECT_COLUMN:
        case OBJECT_ATTRIBUTE:
            renameatt(stmt);
            break;

        case OBJECT_TRIGGER:
            renametrig(stmt);
            break;

        case OBJECT_TSPARSER:
            RenameTSParser(stmt->object, stmt->newname);
            break;

        case OBJECT_TSDICTIONARY:
            RenameTSDictionary(stmt->object, stmt->newname);
            break;

        case OBJECT_TSTEMPLATE:
            RenameTSTemplate(stmt->object, stmt->newname);
            break;

        case OBJECT_TSCONFIGURATION:
            RenameTSConfiguration(stmt->object, stmt->newname);
            break;

        case OBJECT_DOMAIN:
        case OBJECT_TYPE:
            RenameType(stmt);
            break;

        case OBJECT_DATA_SOURCE:
            RenameDataSource(stmt->subname, stmt->newname);
            break;

        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized rename stmt type: %d", (int)stmt->renameType)));
    }
}

/*
 * Executes an ALTER OBJECT / SET SCHEMA statement.  Based on the object
 * type, the function appropriate to that type is executed.
 */
void ExecAlterObjectSchemaStmt(AlterObjectSchemaStmt* stmt)
{
    switch (stmt->objectType) {
        case OBJECT_AGGREGATE:
            /* Given ordered set aggregate with no direct args, aggr_args variable is modified in gram.y.
               So the parse of aggr_args should be changed. See gram.y for detail. */
            stmt->objarg = (List*)linitial(stmt->objarg);
            AlterFunctionNamespace(stmt->object, stmt->objarg, true, stmt->newschema);
            break;

        case OBJECT_COLLATION:
            AlterCollationNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_CONVERSION:
            AlterConversionNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_EXTENSION:
            AlterExtensionNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_FUNCTION:
            AlterFunctionNamespace(stmt->object, stmt->objarg, false, stmt->newschema);
            break;

        case OBJECT_OPERATOR:
            AlterOperatorNamespace(stmt->object, stmt->objarg, stmt->newschema);
            break;

        case OBJECT_OPCLASS:
            AlterOpClassNamespace(stmt->object, stmt->addname, stmt->newschema);
            break;

        case OBJECT_OPFAMILY:
            AlterOpFamilyNamespace(stmt->object, stmt->addname, stmt->newschema);
            break;

        case OBJECT_SEQUENCE:
        case OBJECT_LARGE_SEQUENCE:
        case OBJECT_TABLE:
        case OBJECT_VIEW:
        case OBJECT_CONTQUERY:
        case OBJECT_MATVIEW:
        case OBJECT_FOREIGN_TABLE:
        case OBJECT_STREAM:
            if (stmt->objectType == OBJECT_STREAM)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Un-support feature"),
                        errdetail("target table is a stream")));
            AlterTableNamespace(stmt);
            break;

        case OBJECT_TSPARSER:
            AlterTSParserNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_TSDICTIONARY:
            AlterTSDictionaryNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_TSTEMPLATE:
            AlterTSTemplateNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_TSCONFIGURATION:
            AlterTSConfigurationNamespace(stmt->object, stmt->newschema);
            break;

        case OBJECT_TYPE:
        case OBJECT_DOMAIN:
            AlterTypeNamespace(stmt->object, stmt->newschema, stmt->objectType);
            break;

        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized AlterObjectSchemaStmt type: %d", (int)stmt->objectType)));
    }
}

/*
 * Change an object's namespace given its classOid and object Oid.
 *
 * Objects that don't have a namespace should be ignored.
 *
 * This function is currently used only by ALTER EXTENSION SET SCHEMA,
 * so it only needs to cover object types that can be members of an
 * extension, and it doesn't have to deal with certain special cases
 * such as not wanting to process array types --- those should never
 * be direct members of an extension anyway.
 *
 * Returns the OID of the object's previous namespace, or InvalidOid if
 * object doesn't have a schema.
 */
Oid AlterObjectNamespace_oid(Oid classId, Oid objid, Oid nspOid, ObjectAddresses* objsMoved)
{
    Oid oldNspOid = InvalidOid;
    ObjectAddress dep;

    dep.classId = classId;
    dep.objectId = objid;
    dep.objectSubId = 0;

    switch (getObjectClass(&dep)) {
        case OCLASS_CLASS: {
            Relation rel;

            rel = relation_open(objid, AccessExclusiveLock);
            oldNspOid = RelationGetNamespace(rel);

            AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);

            relation_close(rel, NoLock);
            break;
        }

        case OCLASS_PROC:
            oldNspOid = AlterFunctionNamespace_oid(objid, nspOid);
            break;

        case OCLASS_TYPE:
            oldNspOid = AlterTypeNamespace_oid(objid, nspOid, objsMoved);
            break;

        case OCLASS_COLLATION:
            oldNspOid = AlterCollationNamespace_oid(objid, nspOid);
            break;

        case OCLASS_CONVERSION:
            oldNspOid = AlterConversionNamespace_oid(objid, nspOid);
            break;

        case OCLASS_OPERATOR:
            oldNspOid = AlterOperatorNamespace_oid(objid, nspOid);
            break;

        case OCLASS_OPCLASS:
            oldNspOid = AlterOpClassNamespace_oid(objid, nspOid);
            break;

        case OCLASS_OPFAMILY:
            oldNspOid = AlterOpFamilyNamespace_oid(objid, nspOid);
            break;

        case OCLASS_TSPARSER:
            oldNspOid = AlterTSParserNamespace_oid(objid, nspOid);
            break;

        case OCLASS_TSDICT:
            oldNspOid = AlterTSDictionaryNamespace_oid(objid, nspOid);
            break;

        case OCLASS_TSTEMPLATE:
            oldNspOid = AlterTSTemplateNamespace_oid(objid, nspOid);
            break;

        case OCLASS_TSCONFIG:
            oldNspOid = AlterTSConfigurationNamespace_oid(objid, nspOid);
            break;

        default:
            break;
    }

    return oldNspOid;
}

/*
 * Generic function to change the namespace of a given object, for simple
 * cases (won't work for tables, nor other cases where we need to do more
 * than change the namespace column of a single catalog entry).
 *
 * The AlterFooNamespace() calls just above will call a function whose job
 * is to lookup the arguments for the generic function here.
 *
 * rel: catalog relation containing object (RowExclusiveLock'd by caller)
 * oidCacheId: syscache that indexes this catalog by OID
 * nameCacheId: syscache that indexes this catalog by name and namespace
 *		(pass -1 if there is none)
 * objid: OID of object to change the namespace of
 * nspOid: OID of new namespace
 * Anum_name: column number of catalog's name column
 * Anum_namespace: column number of catalog's namespace column
 * Anum_owner: column number of catalog's owner column, or -1 if none
 * acl_kind: ACL type for object, or -1 if none assigned
 *
 * If the object does not have an owner or permissions, pass -1 for
 * Anum_owner and acl_kind.  In this case the calling user must be superuser.
 *
 * Returns the OID of the object's previous namespace.
 */
Oid AlterObjectNamespace(Relation rel, int oidCacheId, int nameCacheId, Oid objid, Oid nspOid, int Anum_name,
    int Anum_namespace, int Anum_owner, AclObjectKind acl_kind)
{
    Oid classId = RelationGetRelid(rel);
    Oid oldNspOid;
    Datum name, nmspace;
    bool isnull = false;
    HeapTuple tup, newtup;
    Datum* values = NULL;
    bool* nulls = NULL;
    bool* replaces = NULL;

    tup = SearchSysCacheCopy1(oidCacheId, ObjectIdGetDatum(objid));
    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for object %u of catalog \"%s\"", objid, RelationGetRelationName(rel))));

    // AM_TODO: system table access, not necessary to use API
    name = tableam_tops_tuple_getattr(tup, Anum_name, RelationGetDescr(rel), &isnull);
    Assert(!isnull);
    nmspace = heap_getattr(tup, Anum_namespace, RelationGetDescr(rel), &isnull);
    Assert(!isnull);
    oldNspOid = DatumGetObjectId(nmspace);

    /* Check basic namespace related issues */
    CheckSetNamespace(oldNspOid, nspOid, classId, objid);

    /* Permission checks ... superusers can always do it */
    /* Database Security:  Support separation of privilege. */
    if (!isRelSuperuser()) {
        Datum owner;
        Oid ownerId;
        AclResult aclresult;

        /* Fail if object does not have an explicit owner */
        if (Anum_owner <= 0)
            ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    (errmsg("must be system admin to set schema of %s", getObjectDescriptionOids(classId, objid)))));

        /* Otherwise, must be owner of the existing object */
        owner = heap_getattr(tup, Anum_owner, RelationGetDescr(rel), &isnull);
        Assert(!isnull);
        ownerId = DatumGetObjectId(owner);
        if (!has_privs_of_role(GetUserId(), ownerId))
            aclcheck_error(ACLCHECK_NOT_OWNER, acl_kind, NameStr(*(DatumGetName(name))));

        /* User must have CREATE privilege on new namespace */
        aclresult = pg_namespace_aclcheck(nspOid, GetUserId(), ACL_CREATE);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(nspOid));
    }

    /*
     * Check for duplicate name (more friendly than unique-index failure).
     * Since this is just a friendliness check, we can just skip it in cases
     * where there isn't a suitable syscache available.
     */
    if (nameCacheId >= 0 && SearchSysCacheExists2(nameCacheId, name, ObjectIdGetDatum(nspOid)))
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT),
                errmsg("%s already exists in schema \"%s\"",
                    getObjectDescriptionOids(classId, objid),
                    get_namespace_name(nspOid))));

    /* Build modified tuple */
    values = (Datum*)palloc0(RelationGetNumberOfAttributes(rel) * sizeof(Datum));
    nulls = (bool*)palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
    replaces = (bool*)palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
    values[Anum_namespace - 1] = ObjectIdGetDatum(nspOid);
    replaces[Anum_namespace - 1] = true;
    newtup = (HeapTuple) tableam_tops_modify_tuple(tup, RelationGetDescr(rel), values, nulls, replaces);

    /* Perform actual update */
    simple_heap_update(rel, &tup->t_self, newtup);
    CatalogUpdateIndexes(rel, newtup);

    /* Release memory */
    pfree_ext(values);
    pfree_ext(nulls);
    pfree_ext(replaces);

    /* update dependencies to point to the new schema */
    (void)changeDependencyFor(classId, objid, NamespaceRelationId, oldNspOid, nspOid);

    return oldNspOid;
}

/*
 * Executes an ALTER OBJECT / OWNER TO statement.  Based on the object
 * type, the function appropriate to that type is executed.
 */
void ExecAlterOwnerStmt(AlterOwnerStmt* stmt)
{
    const char* newOwnerName = stmt->newowner;
    Oid newowner;
    if (strcmp(newOwnerName, "current_user") == 0) {
        /* CURRENT_USER */
        newowner = GetUserId();
    } else if (strcmp(newOwnerName, "session_user") == 0) {
        /* SESSION_USER */
        newowner = GetSessionUserId();
    } else {
        /* Normal User */
        newowner = get_role_oid(newOwnerName, false);
    }

    switch (stmt->objectType) {
        case OBJECT_AGGREGATE:
            /* Given ordered set aggregate with no direct args, aggr_args variable is modified in gram.y.
               So the parse of aggr_args should be changed. See gram.y for detail. */
            stmt->objarg = (List*)linitial(stmt->objarg);
            AlterAggregateOwner(stmt->object, stmt->objarg, newowner);
            break;

        case OBJECT_COLLATION:
            AlterCollationOwner(stmt->object, newowner);
            break;

        case OBJECT_CONVERSION:
            AlterConversionOwner(stmt->object, newowner);
            break;

        case OBJECT_DATABASE:
            AlterDatabaseOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_FUNCTION:
            AlterFunctionOwner(stmt->object, stmt->objarg, newowner);
            break;

        case OBJECT_PACKAGE:
            AlterPackageOwner(stmt->object, newowner);
            break;
        case OBJECT_LANGUAGE:
            AlterLanguageOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_LARGEOBJECT:
            LargeObjectAlterOwner(oidparse((Node*)linitial(stmt->object)), newowner);
            break;

        case OBJECT_OPERATOR:
            Assert(list_length(stmt->objarg) == 2);
            AlterOperatorOwner(
                stmt->object, (TypeName*)linitial(stmt->objarg), (TypeName*)lsecond(stmt->objarg), newowner);
            break;

        case OBJECT_OPCLASS:
            AlterOpClassOwner(stmt->object, stmt->addname, newowner);
            break;

        case OBJECT_OPFAMILY:
            AlterOpFamilyOwner(stmt->object, stmt->addname, newowner);
            break;

        case OBJECT_SCHEMA:
            AlterSchemaOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_TABLESPACE:
            AlterTableSpaceOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_TYPE:
        case OBJECT_DOMAIN: /* same as TYPE */
            AlterTypeOwner(stmt->object, newowner, stmt->objectType, true);
            break;

        case OBJECT_TSDICTIONARY:
            AlterTSDictionaryOwner(stmt->object, newowner);
            break;

        case OBJECT_TSCONFIGURATION:
            AlterTSConfigurationOwner(stmt->object, newowner);
            break;

        case OBJECT_FDW:
            AlterForeignDataWrapperOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_FOREIGN_SERVER:
            AlterForeignServerOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_DATA_SOURCE:
            AlterDataSourceOwner(strVal(linitial(stmt->object)), newowner);
            break;
        case OBJECT_DIRECTORY:
            AlterDirectoryOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_SYNONYM:
            AlterSynonymOwner(stmt->object, newowner);
            break;

        case OBJECT_PUBLICATION:
            AlterPublicationOwner(strVal(linitial(stmt->object)), newowner);
            break;

        case OBJECT_SUBSCRIPTION:
            AlterSubscriptionOwner(strVal(linitial(stmt->object)), newowner);
            break;

        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized AlterOwnerStmt type: %d", (int)stmt->objectType)));
    }
}
