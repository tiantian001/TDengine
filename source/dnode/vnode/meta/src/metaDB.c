/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "metaDef.h"

static void metaSaveSchemaDB(SMeta *pMeta, tb_uid_t uid, STSchema *pSchema);
static void metaGetSchemaDBKey(char key[], tb_uid_t uid, int sversion);
// static int  metaSaveMapDB(SMeta *pMeta, tb_uid_t suid, tb_uid_t uid);

#define SCHEMA_KEY_LEN (sizeof(tb_uid_t) + sizeof(int))

#define META_OPEN_DB_IMPL(pDB, options, dir, err) \
  do {                                            \
    pDB = rocksdb_open(options, dir, &err);       \
    if (pDB == NULL) {                            \
      metaCloseDB(pMeta);                         \
      rocksdb_options_destroy(options);           \
      return -1;                                  \
    }                                             \
  } while (0)

int metaOpenDB(SMeta *pMeta) {
  char               dir[128];
  char *             err = NULL;
  rocksdb_options_t *options = rocksdb_options_create();

  if (pMeta->pCache) {
    rocksdb_options_set_row_cache(options, pMeta->pCache);
  }
  rocksdb_options_set_create_if_missing(options, 1);

  pMeta->pDB = (meta_db_t *)calloc(1, sizeof(*(pMeta->pDB)));
  if (pMeta->pDB == NULL) {
    // TODO: handle error
    rocksdb_options_destroy(options);
    return -1;
  }

  // tbDb
  sprintf(dir, "%s/tb_db", pMeta->path);
  META_OPEN_DB_IMPL(pMeta->pDB->tbDb, options, dir, err);

  // nameDb
  sprintf(dir, "%s/name_db", pMeta->path);
  META_OPEN_DB_IMPL(pMeta->pDB->nameDb, options, dir, err);

  // tagDb
  sprintf(dir, "%s/tag_db", pMeta->path);
  META_OPEN_DB_IMPL(pMeta->pDB->tagDb, options, dir, err);

  // schemaDb
  sprintf(dir, "%s/schema_db", pMeta->path);
  META_OPEN_DB_IMPL(pMeta->pDB->schemaDb, options, dir, err);

  // mapDb
  sprintf(dir, "%s/meta.db", pMeta->path);
  if (sqlite3_open(dir, &(pMeta->pDB->mapDb)) != SQLITE_OK) {
    // TODO
  }

  // // set read uncommitted
  sqlite3_exec(pMeta->pDB->mapDb, "PRAGMA read_uncommitted=true;", 0, 0, 0);
  sqlite3_exec(pMeta->pDB->mapDb, "BEGIN;", 0, 0, 0);

  rocksdb_options_destroy(options);
  return 0;
}

#define META_CLOSE_DB_IMPL(pDB) \
  do {                          \
    if (pDB) {                  \
      rocksdb_close(pDB);       \
      pDB = NULL;               \
    }                           \
  } while (0)

void metaCloseDB(SMeta *pMeta) {
  if (pMeta->pDB) {
    if (pMeta->pDB->mapDb) {
      sqlite3_exec(pMeta->pDB->mapDb, "COMMIT;", 0, 0, 0);
      sqlite3_close(pMeta->pDB->mapDb);
      pMeta->pDB->mapDb = NULL;
    }

    META_CLOSE_DB_IMPL(pMeta->pDB->schemaDb);
    META_CLOSE_DB_IMPL(pMeta->pDB->tagDb);
    META_CLOSE_DB_IMPL(pMeta->pDB->nameDb);
    META_CLOSE_DB_IMPL(pMeta->pDB->tbDb);
    free(pMeta->pDB);
    pMeta->pDB = NULL;
  }
}

int metaSaveTableToDB(SMeta *pMeta, const STbCfg *pTbOptions) {
  tb_uid_t uid;
  char *   err = NULL;
  size_t   size;
  char     pBuf[1024];  // TODO
  char     sql[128];

  rocksdb_writeoptions_t *wopt = rocksdb_writeoptions_create();

  // Generate a uid for child and normal table
  if (pTbOptions->type == META_SUPER_TABLE) {
    uid = pTbOptions->stbCfg.suid;
  } else {
    uid = metaGenerateUid(pMeta);
  }

  // Save tbname -> uid to tbnameDB
  rocksdb_put(pMeta->pDB->nameDb, wopt, pTbOptions->name, strlen(pTbOptions->name), (char *)(&uid), sizeof(uid), &err);
  rocksdb_writeoptions_disable_WAL(wopt, 1);

  // Save uid -> tb_obj to tbDB
  size = metaEncodeTbObjFromTbOptions(pTbOptions, pBuf, 1024);
  rocksdb_put(pMeta->pDB->tbDb, wopt, (char *)(&uid), sizeof(uid), pBuf, size, &err);

  switch (pTbOptions->type) {
    case META_NORMAL_TABLE:
      // save schemaDB
      metaSaveSchemaDB(pMeta, uid, pTbOptions->ntbCfg.pSchema);
      break;
    case META_SUPER_TABLE:
      // save schemaDB
      metaSaveSchemaDB(pMeta, uid, pTbOptions->stbCfg.pSchema);

      // // save mapDB (really need?)
      // rocksdb_put(pMeta->pDB->mapDb, wopt, (char *)(&uid), sizeof(uid), "", 0, &err);
      sprintf(sql, "create table st_%" PRIu64 " (uid BIGINT);", uid);
      if (sqlite3_exec(pMeta->pDB->mapDb, sql, NULL, NULL, &err) != SQLITE_OK) {
        // fprintf(stderr, "Failed to create table, since %s\n", err);
      }
      break;
    case META_CHILD_TABLE:
      // save tagDB
      rocksdb_put(pMeta->pDB->tagDb, wopt, (char *)(&uid), sizeof(uid), pTbOptions->ctbCfg.pTag,
                  kvRowLen(pTbOptions->ctbCfg.pTag), &err);

      // save mapDB
      sprintf(sql, "insert into st_%" PRIu64 " values (%" PRIu64 ");", pTbOptions->ctbCfg.suid, uid);
      if (sqlite3_exec(pMeta->pDB->mapDb, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "failed to insert data, since %s\n", err);
      }
      break;
    default:
      ASSERT(0);
  }

  rocksdb_writeoptions_destroy(wopt);

  return 0;
}

int metaRemoveTableFromDb(SMeta *pMeta, tb_uid_t uid) {
  /* TODO */
  return 0;
}

/* ------------------------ STATIC METHODS ------------------------ */
static void metaSaveSchemaDB(SMeta *pMeta, tb_uid_t uid, STSchema *pSchema) {
  char   key[64];
  char   pBuf[1024];
  char * ppBuf = pBuf;
  size_t vsize;
  char * err = NULL;

  rocksdb_writeoptions_t *wopt = rocksdb_writeoptions_create();
  rocksdb_writeoptions_disable_WAL(wopt, 1);

  metaGetSchemaDBKey(key, uid, schemaVersion(pSchema));
  vsize = tdEncodeSchema((void **)(&ppBuf), pSchema);
  rocksdb_put(pMeta->pDB->schemaDb, wopt, key, SCHEMA_KEY_LEN, pBuf, vsize, &err);

  rocksdb_writeoptions_destroy(wopt);
}

static void metaGetSchemaDBKey(char *key, tb_uid_t uid, int sversion) {
  *(tb_uid_t *)key = uid;
  *(int *)POINTER_SHIFT(key, sizeof(tb_uid_t)) = sversion;
}

// static int metaSaveMapDB(SMeta *pMeta, tb_uid_t suid, tb_uid_t uid) {
//   size_t vlen;
//   char * val;
//   char * err = NULL;

//   rocksdb_readoptions_t *ropt = rocksdb_readoptions_create();
//   val = rocksdb_get(pMeta->pDB->mapDb, ropt, (char *)(&suid), sizeof(suid), &vlen, &err);
//   rocksdb_readoptions_destroy(ropt);

//   void *nval = malloc(vlen + sizeof(uid));
//   if (nval == NULL) {
//     return -1;
//   }

//   if (vlen) {
//     memcpy(nval, val, vlen);
//   }
//   memcpy(POINTER_SHIFT(nval, vlen), (void *)(&uid), sizeof(uid));

//   rocksdb_writeoptions_t *wopt = rocksdb_writeoptions_create();
//   rocksdb_writeoptions_disable_WAL(wopt, 1);

//   rocksdb_put(pMeta->pDB->mapDb, wopt, (char *)(&suid), sizeof(suid), nval, vlen + sizeof(uid), &err);

//   rocksdb_writeoptions_destroy(wopt);
//   free(nval);

//   return 0;
// }