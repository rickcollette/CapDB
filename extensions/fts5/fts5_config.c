/*
** 2014 Jun 09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This is an SQLite module implementing full-text search.
*/


#include "fts5Int.h"

#define FTS5_DEFAULT_PAGE_SIZE   4050
#define FTS5_DEFAULT_AUTOMERGE      4
#define FTS5_DEFAULT_USERMERGE      4
#define FTS5_DEFAULT_CRISISMERGE   16
#define FTS5_DEFAULT_HASHSIZE    (1024*1024)

#define FTS5_DEFAULT_DELETE_AUTOMERGE 10      /* default 10% */

/* Maximum allowed page size */
#define FTS5_MAX_PAGE_SIZE (64*1024)

static int fts5_iswhitespace(char x){
  return (x==' ');
}

static int fts5_isopenquote(char x){
  return (x=='"' || x=='\'' || x=='[' || x=='`');
}

/*
** Argument pIn points to a character that is part of a nul-terminated 
** string. Return a pointer to the first character following *pIn in 
** the string that is not a white-space character.
*/
static const char *fts5ConfigSkipWhitespace(const char *pIn){
  const char *p = pIn;
  if( p ){
    while( fts5_iswhitespace(*p) ){ p++; }
  }
  return p;
}

/*
** Argument pIn points to a character that is part of a nul-terminated 
** string. Return a pointer to the first character following *pIn in 
** the string that is not a "bareword" character.
*/
static const char *fts5ConfigSkipBareword(const char *pIn){
  const char *p = pIn;
  while ( capdbFts5IsBareword(*p) ) p++;
  if( p==pIn ) p = 0;
  return p;
}

static int fts5_isdigit(char a){
  return (a>='0' && a<='9');
}



static const char *fts5ConfigSkipLiteral(const char *pIn){
  const char *p = pIn;
  switch( *p ){
    case 'n': case 'N':
      if( capdb_strnicmp("null", p, 4)==0 ){
        p = &p[4];
      }else{
        p = 0;
      }
      break;

    case 'x': case 'X':
      p++;
      if( *p=='\'' ){
        p++;
        while( (*p>='a' && *p<='f') 
            || (*p>='A' && *p<='F') 
            || (*p>='0' && *p<='9') 
            ){
          p++;
        }
        if( *p=='\'' && 0==((p-pIn)%2) ){
          p++;
        }else{
          p = 0;
        }
      }else{
        p = 0;
      }
      break;

    case '\'':
      p++;
      while( p ){
        if( *p=='\'' ){
          p++;
          if( *p!='\'' ) break;
        }
        p++;
        if( *p==0 ) p = 0;
      }
      break;

    default:
      /* maybe a number */
      if( *p=='+' || *p=='-' ) p++;
      while( fts5_isdigit(*p) ) p++;

      /* At this point, if the literal was an integer, the parse is 
      ** finished. Or, if it is a floating point value, it may continue
      ** with either a decimal point or an 'E' character. */
      if( *p=='.' && fts5_isdigit(p[1]) ){
        p += 2;
        while( fts5_isdigit(*p) ) p++;
      }
      if( p==pIn ) p = 0;

      break;
  }

  return p;
}

/*
** The first character of the string pointed to by argument z is guaranteed
** to be an open-quote character (see function fts5_isopenquote()).
**
** This function searches for the corresponding close-quote character within
** the string and, if found, dequotes the string in place and adds a new
** nul-terminator byte.
**
** If the close-quote is found, the value returned is the byte offset of
** the character immediately following it. Or, if the close-quote is not 
** found, -1 is returned. If -1 is returned, the buffer is left in an 
** undefined state.
*/
static int fts5Dequote(char *z){
  char q;
  int iIn = 1;
  int iOut = 0;
  q = z[0];

  /* Set stack variable q to the close-quote character */
  assert( q=='[' || q=='\'' || q=='"' || q=='`' );
  if( q=='[' ) q = ']';  

  while( z[iIn] ){
    if( z[iIn]==q ){
      if( z[iIn+1]!=q ){
        /* Character iIn was the close quote. */
        iIn++;
        break;
      }else{
        /* Character iIn and iIn+1 form an escaped quote character. Skip
        ** the input cursor past both and copy a single quote character 
        ** to the output buffer. */
        iIn += 2;
        z[iOut++] = q;
      }
    }else{
      z[iOut++] = z[iIn++];
    }
  }

  z[iOut] = '\0';
  return iIn;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
void capdbFts5Dequote(char *z){
  char quote;                     /* Quote character (if any ) */

  assert( 0==fts5_iswhitespace(z[0]) );
  quote = z[0];
  if( quote=='[' || quote=='\'' || quote=='"' || quote=='`' ){
    fts5Dequote(z);
  }
}


struct Fts5Enum {
  const char *zName;
  int eVal;
};
typedef struct Fts5Enum Fts5Enum;

static int fts5ConfigSetEnum(
  const Fts5Enum *aEnum, 
  const char *zEnum, 
  int *peVal
){
  int nEnum = (int)strlen(zEnum);
  int i;
  int iVal = -1;

  for(i=0; aEnum[i].zName; i++){
    if( capdb_strnicmp(aEnum[i].zName, zEnum, nEnum)==0 ){
      if( iVal>=0 ) return CAPDB_ERROR;
      iVal = aEnum[i].eVal;
    }
  }

  *peVal = iVal;
  return iVal<0 ? CAPDB_ERROR : CAPDB_OK;
}

/*
** Parse a "special" CREATE VIRTUAL TABLE directive and update
** configuration object pConfig as appropriate.
**
** If successful, object pConfig is updated and CAPDB_OK returned. If
** an error occurs, an SQLite error code is returned and an error message
** may be left in *pzErr. It is the responsibility of the caller to
** eventually free any such error message using capdb_free().
*/
static int fts5ConfigParseSpecial(
  Fts5Config *pConfig,            /* Configuration object to update */
  const char *zCmd,               /* Special command to parse */
  const char *zArg,               /* Argument to parse */
  char **pzErr                    /* OUT: Error message */
){
  int rc = CAPDB_OK;
  int nCmd = (int)strlen(zCmd);

  if( capdb_strnicmp("prefix", zCmd, nCmd)==0 ){
    const int nByte = sizeof(int) * FTS5_MAX_PREFIX_INDEXES;
    const char *p;
    int bFirst = 1;
    if( pConfig->aPrefix==0 ){
      pConfig->aPrefix = capdbFts5MallocZero(&rc, nByte);
      if( rc ) return rc;
    }

    p = zArg;
    while( 1 ){
      int nPre = 0;

      while( p[0]==' ' ) p++;
      if( bFirst==0 && p[0]==',' ){
        p++;
        while( p[0]==' ' ) p++;
      }else if( p[0]=='\0' ){
        break;
      }
      if( p[0]<'0' || p[0]>'9' ){
        *pzErr = capdb_mprintf("malformed prefix=... directive");
        rc = CAPDB_ERROR;
        break;
      }

      if( pConfig->nPrefix==FTS5_MAX_PREFIX_INDEXES ){
        *pzErr = capdb_mprintf(
            "too many prefix indexes (max %d)", FTS5_MAX_PREFIX_INDEXES
        );
        rc = CAPDB_ERROR;
        break;
      }

      while( p[0]>='0' && p[0]<='9' && nPre<1000 ){
        nPre = nPre*10 + (p[0] - '0');
        p++;
      }

      if( nPre<=0 || nPre>=1000 ){
        *pzErr = capdb_mprintf("prefix length out of range (max 999)");
        rc = CAPDB_ERROR;
        break;
      }

      pConfig->aPrefix[pConfig->nPrefix] = nPre;
      pConfig->nPrefix++;
      bFirst = 0;
    }
    assert( pConfig->nPrefix<=FTS5_MAX_PREFIX_INDEXES );
    return rc;
  }

  if( capdb_strnicmp("tokenize", zCmd, nCmd)==0 ){
    const char *p = (const char*)zArg;
    capdb_int64 nArg = strlen(zArg) + 1;
    char **azArg = capdbFts5MallocZero(&rc, (sizeof(char*) + 2) * nArg);

    if( azArg ){
      char *pSpace = (char*)&azArg[nArg];
      if( pConfig->t.azArg ){
        *pzErr = capdb_mprintf("multiple tokenize=... directives");
        rc = CAPDB_ERROR;
      }else{
        for(nArg=0; p && *p; nArg++){
          const char *p2 = fts5ConfigSkipWhitespace(p);
          if( *p2=='\'' ){
            p = fts5ConfigSkipLiteral(p2);
          }else{
            p = fts5ConfigSkipBareword(p2);
          }
          if( p ){
            memcpy(pSpace, p2, p-p2);
            azArg[nArg] = pSpace;
            capdbFts5Dequote(pSpace);
            pSpace += (p - p2) + 1;
            p = fts5ConfigSkipWhitespace(p);
          }
        }
        if( p==0 ){
          *pzErr = capdb_mprintf("parse error in tokenize directive");
          rc = CAPDB_ERROR;
        }else{
          pConfig->t.azArg = (const char**)azArg;
          pConfig->t.nArg = nArg;
          azArg = 0;
        }
      }
    }
    capdb_free(azArg);

    return rc;
  }

  if( capdb_strnicmp("content", zCmd, nCmd)==0 ){
    if( pConfig->eContent!=FTS5_CONTENT_NORMAL ){
      *pzErr = capdb_mprintf("multiple content=... directives");
      rc = CAPDB_ERROR;
    }else{
      if( zArg[0] ){
        pConfig->eContent = FTS5_CONTENT_EXTERNAL;
        pConfig->zContent = capdbFts5Mprintf(&rc, "%Q.%Q", pConfig->zDb,zArg);
      }else{
        pConfig->eContent = FTS5_CONTENT_NONE;
      }
    }
    return rc;
  }

  if( capdb_strnicmp("contentless_delete", zCmd, nCmd)==0 ){
    if( (zArg[0]!='0' && zArg[0]!='1') || zArg[1]!='\0' ){
      *pzErr = capdb_mprintf("malformed contentless_delete=... directive");
      rc = CAPDB_ERROR;
    }else{
      pConfig->bContentlessDelete = (zArg[0]=='1');
    }
    return rc;
  }

  if( capdb_strnicmp("contentless_unindexed", zCmd, nCmd)==0 ){
    if( (zArg[0]!='0' && zArg[0]!='1') || zArg[1]!='\0' ){
      *pzErr = capdb_mprintf("malformed contentless_delete=... directive");
      rc = CAPDB_ERROR;
    }else{
      pConfig->bContentlessUnindexed = (zArg[0]=='1');
    }
    return rc;
  }

  if( capdb_strnicmp("content_rowid", zCmd, nCmd)==0 ){
    if( pConfig->zContentRowid ){
      *pzErr = capdb_mprintf("multiple content_rowid=... directives");
      rc = CAPDB_ERROR;
    }else{
      pConfig->zContentRowid = capdbFts5Strndup(&rc, zArg, -1);
    }
    return rc;
  }

  if( capdb_strnicmp("columnsize", zCmd, nCmd)==0 ){
    if( (zArg[0]!='0' && zArg[0]!='1') || zArg[1]!='\0' ){
      *pzErr = capdb_mprintf("malformed columnsize=... directive");
      rc = CAPDB_ERROR;
    }else{
      pConfig->bColumnsize = (zArg[0]=='1');
    }
    return rc;
  }

  if( capdb_strnicmp("locale", zCmd, nCmd)==0 ){
    if( (zArg[0]!='0' && zArg[0]!='1') || zArg[1]!='\0' ){
      *pzErr = capdb_mprintf("malformed locale=... directive");
      rc = CAPDB_ERROR;
    }else{
      pConfig->bLocale = (zArg[0]=='1');
    }
    return rc;
  }

  if( capdb_strnicmp("detail", zCmd, nCmd)==0 ){
    const Fts5Enum aDetail[] = {
      { "none", FTS5_DETAIL_NONE },
      { "full", FTS5_DETAIL_FULL },
      { "columns", FTS5_DETAIL_COLUMNS },
      { 0, 0 }
    };

    if( (rc = fts5ConfigSetEnum(aDetail, zArg, &pConfig->eDetail)) ){
      *pzErr = capdb_mprintf("malformed detail=... directive");
    }
    return rc;
  }

  if( capdb_strnicmp("tokendata", zCmd, nCmd)==0 ){
    if( (zArg[0]!='0' && zArg[0]!='1') || zArg[1]!='\0' ){
      *pzErr = capdb_mprintf("malformed tokendata=... directive");
      rc = CAPDB_ERROR;
    }else{
      pConfig->bTokendata = (zArg[0]=='1');
    }
    return rc;
  }

  *pzErr = capdb_mprintf("unrecognized option: \"%.*s\"", nCmd, zCmd);
  return CAPDB_ERROR;
}

/*
** Gobble up the first bareword or quoted word from the input buffer zIn.
** Return a pointer to the character immediately following the last in
** the gobbled word if successful, or a NULL pointer otherwise (failed
** to find close-quote character).
**
** Before returning, set pzOut to point to a new buffer containing a
** nul-terminated, dequoted copy of the gobbled word. If the word was
** quoted, *pbQuoted is also set to 1 before returning.
**
** If *pRc is other than CAPDB_OK when this function is called, it is
** a no-op (NULL is returned). Otherwise, if an OOM occurs within this
** function, *pRc is set to CAPDB_NOMEM before returning. *pRc is *not*
** set if a parse error (failed to find close quote) occurs.
*/
static const char *fts5ConfigGobbleWord(
  int *pRc,                       /* IN/OUT: Error code */
  const char *zIn,                /* Buffer to gobble string/bareword from */
  char **pzOut,                   /* OUT: malloc'd buffer containing str/bw */
  int *pbQuoted                   /* OUT: Set to true if dequoting required */
){
  const char *zRet = 0;

  capdb_int64 nIn = strlen(zIn);
  char *zOut = capdb_malloc64(nIn+1);

  assert( *pRc==CAPDB_OK );
  *pbQuoted = 0;
  *pzOut = 0;

  if( zOut==0 ){
    *pRc = CAPDB_NOMEM;
  }else{
    memcpy(zOut, zIn, (size_t)(nIn+1));
    if( fts5_isopenquote(zOut[0]) ){
      int ii = fts5Dequote(zOut);
      zRet = &zIn[ii];
      *pbQuoted = 1;
    }else{
      zRet = fts5ConfigSkipBareword(zIn);
      if( zRet ){
        zOut[zRet-zIn] = '\0';
      }
    }
  }

  if( zRet==0 ){
    capdb_free(zOut);
  }else{
    *pzOut = zOut;
  }

  return zRet;
}

static int fts5ConfigParseColumn(
  Fts5Config *p, 
  char *zCol, 
  char *zArg, 
  char **pzErr,
  int *pbUnindexed
){
  int rc = CAPDB_OK;
  if( 0==capdb_stricmp(zCol, FTS5_RANK_NAME) 
   || 0==capdb_stricmp(zCol, FTS5_ROWID_NAME) 
  ){
    *pzErr = capdb_mprintf("reserved fts5 column name: %s", zCol);
    rc = CAPDB_ERROR;
  }else if( zArg ){
    if( 0==capdb_stricmp(zArg, "unindexed") ){
      p->abUnindexed[p->nCol] = 1;
      *pbUnindexed = 1;
    }else{
      *pzErr = capdb_mprintf("unrecognized column option: %s", zArg);
      rc = CAPDB_ERROR;
    }
  }

  p->azCol[p->nCol++] = zCol;
  return rc;
}

/*
** Populate the Fts5Config.zContentExprlist string.
*/
static int fts5ConfigMakeExprlist(Fts5Config *p){
  int i;
  int rc = CAPDB_OK;
  Fts5Buffer buf = {0, 0, 0};

  capdbFts5BufferAppendPrintf(&rc, &buf, "T.%Q", p->zContentRowid);
  if( p->eContent!=FTS5_CONTENT_NONE ){
    assert( p->eContent==FTS5_CONTENT_EXTERNAL
         || p->eContent==FTS5_CONTENT_NORMAL
         || p->eContent==FTS5_CONTENT_UNINDEXED
    );
    for(i=0; i<p->nCol; i++){
      if( p->eContent==FTS5_CONTENT_EXTERNAL ){
        capdbFts5BufferAppendPrintf(&rc, &buf, ", T.%Q", p->azCol[i]);
      }else if( p->eContent==FTS5_CONTENT_NORMAL || p->abUnindexed[i] ){
        capdbFts5BufferAppendPrintf(&rc, &buf, ", T.c%d", i);
      }else{
        capdbFts5BufferAppendPrintf(&rc, &buf, ", NULL");
      }
    }
  }
  if( p->eContent==FTS5_CONTENT_NORMAL && p->bLocale ){
    for(i=0; i<p->nCol; i++){
      if( p->abUnindexed[i]==0 ){
        capdbFts5BufferAppendPrintf(&rc, &buf, ", T.l%d", i);
      }else{
        capdbFts5BufferAppendPrintf(&rc, &buf, ", NULL");
      }
    }
  }

  assert( p->zContentExprlist==0 );
  p->zContentExprlist = (char*)buf.p;
  return rc;
}

/*
** Arguments nArg/azArg contain the string arguments passed to the xCreate
** or xConnect method of the virtual table. This function attempts to 
** allocate an instance of Fts5Config containing the results of parsing
** those arguments.
**
** If successful, CAPDB_OK is returned and *ppOut is set to point to the
** new Fts5Config object. If an error occurs, an SQLite error code is 
** returned, *ppOut is set to NULL and an error message may be left in
** *pzErr. It is the responsibility of the caller to eventually free any 
** such error message using capdb_free().
*/
int capdbFts5ConfigParse(
  Fts5Global *pGlobal,
  capdb *db,
  int nArg,                       /* Number of arguments */
  const char **azArg,             /* Array of nArg CREATE VIRTUAL TABLE args */
  Fts5Config **ppOut,             /* OUT: Results of parse */
  char **pzErr                    /* OUT: Error message */
){
  int rc = CAPDB_OK;             /* Return code */
  Fts5Config *pRet;               /* New object to return */
  int i;
  capdb_int64 nByte;
  int bUnindexed = 0;             /* True if there are one or more UNINDEXED */

  *ppOut = pRet = (Fts5Config*)capdb_malloc64(sizeof(Fts5Config));
  if( pRet==0 ) return CAPDB_NOMEM;
  memset(pRet, 0, sizeof(Fts5Config));
  pRet->pGlobal = pGlobal;
  pRet->db = db;
  pRet->iCookie = -1;

  nByte = nArg * (sizeof(char*) + sizeof(u8));
  pRet->azCol = (char**)capdbFts5MallocZero(&rc, nByte);
  pRet->abUnindexed = pRet->azCol ? (u8*)&pRet->azCol[nArg] : 0;
  pRet->zDb = capdbFts5Strndup(&rc, azArg[1], -1);
  pRet->zName = capdbFts5Strndup(&rc, azArg[2], -1);
  pRet->bColumnsize = 1;
  pRet->eDetail = FTS5_DETAIL_FULL;
#ifdef CAPDB_DEBUG
  pRet->bPrefixIndex = 1;
#endif
  if( rc==CAPDB_OK && capdb_stricmp(pRet->zName, FTS5_RANK_NAME)==0 ){
    *pzErr = capdb_mprintf("reserved fts5 table name: %s", pRet->zName);
    rc = CAPDB_ERROR;
  }

  assert( (pRet->abUnindexed && pRet->azCol) || rc!=CAPDB_OK );
  for(i=3; rc==CAPDB_OK && i<nArg; i++){
    const char *zOrig = azArg[i];
    const char *z;
    char *zOne = 0;
    char *zTwo = 0;
    int bOption = 0;
    int bMustBeCol = 0;

    z = fts5ConfigGobbleWord(&rc, zOrig, &zOne, &bMustBeCol);
    z = fts5ConfigSkipWhitespace(z);
    if( z && *z=='=' ){
      bOption = 1;
      assert( zOne!=0 );
      z++;
      if( bMustBeCol ) z = 0;
    }
    z = fts5ConfigSkipWhitespace(z);
    if( z && z[0] ){
      int bDummy;
      z = fts5ConfigGobbleWord(&rc, z, &zTwo, &bDummy);
      if( z && z[0] ) z = 0;
    }

    if( rc==CAPDB_OK ){
      if( z==0 ){
        *pzErr = capdb_mprintf("parse error in \"%s\"", zOrig);
        rc = CAPDB_ERROR;
      }else{
        if( bOption ){
          rc = fts5ConfigParseSpecial(pRet, 
            ALWAYS(zOne)?zOne:"",
            zTwo?zTwo:"",
            pzErr
          );
        }else{
          rc = fts5ConfigParseColumn(pRet, zOne, zTwo, pzErr, &bUnindexed);
          zOne = 0;
        }
      }
    }

    capdb_free(zOne);
    capdb_free(zTwo);
  }

  /* We only allow contentless_delete=1 if the table is indeed contentless. */
  if( rc==CAPDB_OK 
   && pRet->bContentlessDelete 
   && pRet->eContent!=FTS5_CONTENT_NONE 
  ){
    *pzErr = capdb_mprintf(
        "contentless_delete=1 requires a contentless table"
    );
    rc = CAPDB_ERROR;
  }

  /* We only allow contentless_delete=1 if columnsize=0 is not present. 
  **
  ** This restriction may be removed at some point. 
  */
  if( rc==CAPDB_OK && pRet->bContentlessDelete && pRet->bColumnsize==0 ){
    *pzErr = capdb_mprintf(
        "contentless_delete=1 is incompatible with columnsize=0"
    );
    rc = CAPDB_ERROR;
  }

  /* We only allow contentless_unindexed=1 if the table is actually a
  ** contentless one.
  */
  if( rc==CAPDB_OK 
   && pRet->bContentlessUnindexed 
   && pRet->eContent!=FTS5_CONTENT_NONE
  ){
    *pzErr = capdb_mprintf(
        "contentless_unindexed=1 requires a contentless table"
    );
    rc = CAPDB_ERROR;
  }

  /* If no zContent option was specified, fill in the default values. */
  if( rc==CAPDB_OK && pRet->zContent==0 ){
    const char *zTail = 0;
    assert( pRet->eContent==FTS5_CONTENT_NORMAL
         || pRet->eContent==FTS5_CONTENT_NONE
    );
    if( pRet->eContent==FTS5_CONTENT_NORMAL ){
      zTail = "content";
    }else if( bUnindexed && pRet->bContentlessUnindexed ){
      pRet->eContent = FTS5_CONTENT_UNINDEXED;
      zTail = "content";
    }else if( pRet->bColumnsize ){
      zTail = "docsize";
    }

    if( zTail ){
      pRet->zContent = capdbFts5Mprintf(
          &rc, "%Q.'%q_%s'", pRet->zDb, pRet->zName, zTail
      );
    }
  }

  if( rc==CAPDB_OK && pRet->zContentRowid==0 ){
    pRet->zContentRowid = capdbFts5Strndup(&rc, "rowid", -1);
  }

  /* Formulate the zContentExprlist text */
  if( rc==CAPDB_OK ){
    rc = fts5ConfigMakeExprlist(pRet);
  }

  if( rc!=CAPDB_OK ){
    capdbFts5ConfigFree(pRet);
    *ppOut = 0;
  }
  return rc;
}

/*
** Free the configuration object passed as the only argument.
*/
void capdbFts5ConfigFree(Fts5Config *pConfig){
  if( pConfig ){
    int i;
    if( pConfig->t.pTok ){
      if( pConfig->t.pApi1 ){
        pConfig->t.pApi1->xDelete(pConfig->t.pTok);
      }else{
        pConfig->t.pApi2->xDelete(pConfig->t.pTok);
      }
    }
    capdb_free((char*)pConfig->t.azArg);
    capdb_free(pConfig->zDb);
    capdb_free(pConfig->zName);
    for(i=0; i<pConfig->nCol; i++){
      capdb_free(pConfig->azCol[i]);
    }
    capdb_free(pConfig->azCol);
    capdb_free(pConfig->aPrefix);
    capdb_free(pConfig->zRank);
    capdb_free(pConfig->zRankArgs);
    capdb_free(pConfig->zContent);
    capdb_free(pConfig->zContentRowid);
    capdb_free(pConfig->zContentExprlist);
    capdb_free(pConfig);
  }
}

/*
** Call capdb_declare_vtab() based on the contents of the configuration
** object passed as the only argument. Return CAPDB_OK if successful, or
** an SQLite error code if an error occurs.
*/
int capdbFts5ConfigDeclareVtab(Fts5Config *pConfig){
  int i;
  int rc = CAPDB_OK;
  char *zSql;

  zSql = capdbFts5Mprintf(&rc, "CREATE TABLE x(");
  for(i=0; zSql && i<pConfig->nCol; i++){
    const char *zSep = (i==0?"":", ");
    zSql = capdbFts5Mprintf(&rc, "%z%s%Q", zSql, zSep, pConfig->azCol[i]);
  }
  zSql = capdbFts5Mprintf(&rc, "%z, %Q HIDDEN, %s HIDDEN)", 
      zSql, pConfig->zName, FTS5_RANK_NAME
  );

  assert( zSql || rc==CAPDB_NOMEM );
  if( zSql ){
    rc = capdb_declare_vtab(pConfig->db, zSql);
    capdb_free(zSql);
  }
 
  return rc;
}

/*
** Tokenize the text passed via the second and third arguments.
**
** The callback is invoked once for each token in the input text. The
** arguments passed to it are, in order:
**
**     void *pCtx          // Copy of 4th argument to capdbFts5Tokenize()
**     const char *pToken  // Pointer to buffer containing token
**     int nToken          // Size of token in bytes
**     int iStart          // Byte offset of start of token within input text
**     int iEnd            // Byte offset of end of token within input text
**     int iPos            // Position of token in input (first token is 0)
**
** If the callback returns a non-zero value the tokenization is abandoned
** and no further callbacks are issued. 
**
** This function returns CAPDB_OK if successful or an SQLite error code
** if an error occurs. If the tokenization was abandoned early because
** the callback returned CAPDB_DONE, this is not an error and this function
** still returns CAPDB_OK. Or, if the tokenization was abandoned early
** because the callback returned another non-zero value, it is assumed
** to be an SQLite error code and returned to the caller.
*/
int capdbFts5Tokenize(
  Fts5Config *pConfig,            /* FTS5 Configuration object */
  int flags,                      /* FTS5_TOKENIZE_* flags */
  const char *pText, int nText,   /* Text to tokenize */
  void *pCtx,                     /* Context passed to xToken() */
  int (*xToken)(void*, int, const char*, int, int, int)    /* Callback */
){
  int rc = CAPDB_OK;
  if( pText ){
    if( pConfig->t.pTok==0 ){
      rc = capdbFts5LoadTokenizer(pConfig);
    }
    if( rc==CAPDB_OK ){
      if( pConfig->t.pApi1 ){
        rc = pConfig->t.pApi1->xTokenize(
            pConfig->t.pTok, pCtx, flags, pText, nText, xToken
        );
      }else{
        rc = pConfig->t.pApi2->xTokenize(pConfig->t.pTok, pCtx, flags, 
            pText, nText, pConfig->t.pLocale, pConfig->t.nLocale, xToken
        );
      }
    }
  }
  return rc;
}

/*
** Argument pIn points to the first character in what is expected to be
** a comma-separated list of SQL literals followed by a ')' character.
** If it actually is this, return a pointer to the ')'. Otherwise, return
** NULL to indicate a parse error.
*/
static const char *fts5ConfigSkipArgs(const char *pIn){
  const char *p = pIn;
  
  while( 1 ){
    p = fts5ConfigSkipWhitespace(p);
    p = fts5ConfigSkipLiteral(p);
    p = fts5ConfigSkipWhitespace(p);
    if( p==0 || *p==')' ) break;
    if( *p!=',' ){
      p = 0;
      break;
    }
    p++;
  }

  return p;
}

/*
** Parameter zIn contains a rank() function specification. The format of 
** this is:
**
**   + Bareword (function name)
**   + Open parenthesis - "("
**   + Zero or more SQL literals in a comma separated list
**   + Close parenthesis - ")"
*/
int capdbFts5ConfigParseRank(
  const char *zIn,                /* Input string */
  char **pzRank,                  /* OUT: Rank function name */
  char **pzRankArgs               /* OUT: Rank function arguments */
){
  const char *p = zIn;
  const char *pRank;
  char *zRank = 0;
  char *zRankArgs = 0;
  int rc = CAPDB_OK;

  *pzRank = 0;
  *pzRankArgs = 0;

  if( p==0 ){
    rc = CAPDB_ERROR;
  }else{
    p = fts5ConfigSkipWhitespace(p);
    pRank = p;
    p = fts5ConfigSkipBareword(p);

    if( p ){
      zRank = capdbFts5MallocZero(&rc, 1 + p - pRank);
      if( zRank ) memcpy(zRank, pRank, p-pRank);
    }else{
      rc = CAPDB_ERROR;
    }

    if( rc==CAPDB_OK ){
      p = fts5ConfigSkipWhitespace(p);
      if( *p!='(' ) rc = CAPDB_ERROR;
      p++;
    }
    if( rc==CAPDB_OK ){
      const char *pArgs; 
      p = fts5ConfigSkipWhitespace(p);
      pArgs = p;
      if( *p!=')' ){
        p = fts5ConfigSkipArgs(p);
        if( p==0 ){
          rc = CAPDB_ERROR;
        }else{
          zRankArgs = capdbFts5MallocZero(&rc, 1 + p - pArgs);
          if( zRankArgs ) memcpy(zRankArgs, pArgs, p-pArgs);
        }
      }
    }
  }

  if( rc!=CAPDB_OK ){
    capdb_free(zRank);
    assert( zRankArgs==0 );
  }else{
    *pzRank = zRank;
    *pzRankArgs = zRankArgs;
  }
  return rc;
}

int capdbFts5ConfigSetValue(
  Fts5Config *pConfig, 
  const char *zKey, 
  capdb_value *pVal,
  int *pbBadkey
){
  int rc = CAPDB_OK;

  if( 0==capdb_stricmp(zKey, "pgsz") ){
    int pgsz = 0;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      pgsz = capdb_value_int(pVal);
    }
    if( pgsz<32 || pgsz>FTS5_MAX_PAGE_SIZE ){
      *pbBadkey = 1;
    }else{
      pConfig->pgsz = pgsz;
    }
  }

  else if( 0==capdb_stricmp(zKey, "hashsize") ){
    int nHashSize = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      nHashSize = capdb_value_int(pVal);
    }
    if( nHashSize<=0 ){
      *pbBadkey = 1;
    }else{
      pConfig->nHashSize = nHashSize;
    }
  }

  else if( 0==capdb_stricmp(zKey, "automerge") ){
    int nAutomerge = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      nAutomerge = capdb_value_int(pVal);
    }
    if( nAutomerge<0 || nAutomerge>64 ){
      *pbBadkey = 1;
    }else{
      if( nAutomerge==1 ) nAutomerge = FTS5_DEFAULT_AUTOMERGE;
      pConfig->nAutomerge = nAutomerge;
    }
  }

  else if( 0==capdb_stricmp(zKey, "usermerge") ){
    int nUsermerge = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      nUsermerge = capdb_value_int(pVal);
    }
    if( nUsermerge<2 || nUsermerge>16 ){
      *pbBadkey = 1;
    }else{
      pConfig->nUsermerge = nUsermerge;
    }
  }

  else if( 0==capdb_stricmp(zKey, "crisismerge") ){
    int nCrisisMerge = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      nCrisisMerge = capdb_value_int(pVal);
    }
    if( nCrisisMerge<0 ){
      *pbBadkey = 1;
    }else{
      if( nCrisisMerge<=1 ) nCrisisMerge = FTS5_DEFAULT_CRISISMERGE;
      if( nCrisisMerge>=FTS5_MAX_SEGMENT ) nCrisisMerge = FTS5_MAX_SEGMENT-1;
      pConfig->nCrisisMerge = nCrisisMerge;
    }
  }

  else if( 0==capdb_stricmp(zKey, "deletemerge") ){
    int nVal = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      nVal = capdb_value_int(pVal);
    }else{
      *pbBadkey = 1;
    }
    if( nVal<0 ) nVal = FTS5_DEFAULT_DELETE_AUTOMERGE;
    if( nVal>100 ) nVal = 0;
    pConfig->nDeleteMerge = nVal;
  }

  else if( 0==capdb_stricmp(zKey, "rank") ){
    const char *zIn = (const char*)capdb_value_text(pVal);
    char *zRank;
    char *zRankArgs;
    rc = capdbFts5ConfigParseRank(zIn, &zRank, &zRankArgs);
    if( rc==CAPDB_OK ){
      capdb_free(pConfig->zRank);
      capdb_free(pConfig->zRankArgs);
      pConfig->zRank = zRank;
      pConfig->zRankArgs = zRankArgs;
    }else if( rc==CAPDB_ERROR ){
      rc = CAPDB_OK;
      *pbBadkey = 1;
    }
  }

  else if( 0==capdb_stricmp(zKey, "secure-delete") ){
    int bVal = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      bVal = capdb_value_int(pVal);
    }
    if( bVal<0 ){
      *pbBadkey = 1;
    }else{
      pConfig->bSecureDelete = (bVal ? 1 : 0);
    }
  }

  else if( 0==capdb_stricmp(zKey, "insttoken") ){
    int bVal = -1;
    if( CAPDB_INTEGER==capdb_value_numeric_type(pVal) ){
      bVal = capdb_value_int(pVal);
    }
    if( bVal<0 ){
      *pbBadkey = 1;
    }else{
      pConfig->bPrefixInsttoken = (bVal ? 1 : 0);
    }

  }else{
    *pbBadkey = 1;
  }
  return rc;
}

/*
** Load the contents of the %_config table into memory.
*/
int capdbFts5ConfigLoad(Fts5Config *pConfig, int iCookie){
  const char *zSelect = "SELECT k, v FROM %Q.'%q_config'";
  char *zSql;
  capdb_stmt *p = 0;
  int rc = CAPDB_OK;
  int iVersion = 0;

  /* Set default values */
  pConfig->pgsz = FTS5_DEFAULT_PAGE_SIZE;
  pConfig->nAutomerge = FTS5_DEFAULT_AUTOMERGE;
  pConfig->nUsermerge = FTS5_DEFAULT_USERMERGE;
  pConfig->nCrisisMerge = FTS5_DEFAULT_CRISISMERGE;
  pConfig->nHashSize = FTS5_DEFAULT_HASHSIZE;
  pConfig->nDeleteMerge = FTS5_DEFAULT_DELETE_AUTOMERGE;

  zSql = capdbFts5Mprintf(&rc, zSelect, pConfig->zDb, pConfig->zName);
  if( zSql ){
    rc = capdb_prepare_v2(pConfig->db, zSql, -1, &p, 0);
    capdb_free(zSql);
  }

  assert( rc==CAPDB_OK || p==0 );
  if( rc==CAPDB_OK ){
    while( CAPDB_ROW==capdb_step(p) ){
      const char *zK = (const char*)capdb_column_text(p, 0);
      capdb_value *pVal = capdb_column_value(p, 1);
      if( 0==capdb_stricmp(zK, "version") ){
        iVersion = capdb_value_int(pVal);
      }else{
        int bDummy = 0;
        capdbFts5ConfigSetValue(pConfig, zK, pVal, &bDummy);
      }
    }
    rc = capdb_finalize(p);
  }
  
  if( rc==CAPDB_OK 
   && iVersion!=FTS5_CURRENT_VERSION
   && iVersion!=FTS5_CURRENT_VERSION_SECUREDELETE
  ){
    rc = CAPDB_ERROR;
    capdbFts5ConfigErrmsg(pConfig, "invalid fts5 file format "
        "(found %d, expected %d or %d) - run 'rebuild'",
        iVersion, FTS5_CURRENT_VERSION, FTS5_CURRENT_VERSION_SECUREDELETE
    );
  }else{
    pConfig->iVersion = iVersion;
  }

  if( rc==CAPDB_OK ){
    pConfig->iCookie = iCookie;
  }
  return rc;
}

/*
** Set (*pConfig->pzErrmsg) to point to an capdb_malloc()ed buffer 
** containing the error message created using printf() style formatting
** string zFmt and its trailing arguments.
*/
void capdbFts5ConfigErrmsg(Fts5Config *pConfig, const char *zFmt, ...){
  va_list ap;                     /* ... printf arguments */
  char *zMsg = 0;

  va_start(ap, zFmt);
  zMsg = capdb_vmprintf(zFmt, ap);
  if( pConfig->pzErrmsg ){
    assert( *pConfig->pzErrmsg==0 );
    *pConfig->pzErrmsg = zMsg;
  }else{
    capdb_free(zMsg);
  }

  va_end(ap);
}
