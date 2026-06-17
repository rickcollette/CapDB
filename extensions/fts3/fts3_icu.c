/*
** 2007 June 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements a tokenizer for fts3 based on the ICU library.
*/
#include "fts3Int.h"
#if !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS3)
#ifdef CAPDB_ENABLE_ICU

#include <assert.h>
#include <string.h>
#include "fts3_tokenizer.h"

#include <unicode/ubrk.h>
#include <unicode/ucol.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>

typedef struct IcuTokenizer IcuTokenizer;
typedef struct IcuCursor IcuCursor;

struct IcuTokenizer {
  capdb_tokenizer base;
  char *zLocale;
};

struct IcuCursor {
  capdb_tokenizer_cursor base;

  UBreakIterator *pIter;      /* ICU break-iterator object */
  int nChar;                  /* Number of UChar elements in pInput */
  UChar *aChar;               /* Copy of input using utf-16 encoding */
  int *aOffset;               /* Offsets of each character in utf-8 input */

  int nBuffer;
  char *zBuffer;

  int iToken;
};

/*
** Create a new tokenizer instance.
*/
static int icuCreate(
  int argc,                            /* Number of entries in argv[] */
  const char * const *argv,            /* Tokenizer creation arguments */
  capdb_tokenizer **ppTokenizer      /* OUT: Created tokenizer */
){
  IcuTokenizer *p;
  int n = 0;

  if( argc>0 ){
    n = strlen(argv[0])+1;
  }
  p = (IcuTokenizer *)capdb_malloc64(sizeof(IcuTokenizer)+n);
  if( !p ){
    return CAPDB_NOMEM;
  }
  memset(p, 0, sizeof(IcuTokenizer));

  if( n ){
    p->zLocale = (char *)&p[1];
    memcpy(p->zLocale, argv[0], n);
  }

  *ppTokenizer = (capdb_tokenizer *)p;

  return CAPDB_OK;
}

/*
** Destroy a tokenizer
*/
static int icuDestroy(capdb_tokenizer *pTokenizer){
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  capdb_free(p);
  return CAPDB_OK;
}

/*
** Prepare to begin tokenizing a particular string.  The input
** string to be tokenized is pInput[0..nBytes-1].  A cursor
** used to incrementally tokenize this string is returned in 
** *ppCursor.
*/
static int icuOpen(
  capdb_tokenizer *pTokenizer,         /* The tokenizer */
  const char *zInput,                    /* Input string */
  int nInput,                            /* Length of zInput in bytes */
  capdb_tokenizer_cursor **ppCursor    /* OUT: Tokenization cursor */
){
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  IcuCursor *pCsr;

  const int32_t opt = U_FOLD_CASE_DEFAULT;
  UErrorCode status = U_ZERO_ERROR;
  int nChar;

  UChar32 c;
  int iInput = 0;
  int iOut = 0;

  *ppCursor = 0;

  if( zInput==0 ){
    nInput = 0;
    zInput = "";
  }else if( nInput<0 ){
    nInput = strlen(zInput);
  }
  nChar = nInput+1;
  pCsr = (IcuCursor *)capdb_malloc64(
      sizeof(IcuCursor) +                /* IcuCursor */
      ((nChar+3)&~3) * sizeof(UChar) +   /* IcuCursor.aChar[] */
      (nChar+1) * sizeof(int)            /* IcuCursor.aOffset[] */
  );
  if( !pCsr ){
    return CAPDB_NOMEM;
  }
  memset(pCsr, 0, sizeof(IcuCursor));
  pCsr->aChar = (UChar *)&pCsr[1];
  pCsr->aOffset = (int *)&pCsr->aChar[(nChar+3)&~3];

  pCsr->aOffset[iOut] = iInput;
  U8_NEXT(zInput, iInput, nInput, c); 
  while( c>0 ){
    int isError = 0;
    c = u_foldCase(c, opt);
    U16_APPEND(pCsr->aChar, iOut, nChar, c, isError);
    if( isError ){
      capdb_free(pCsr);
      return CAPDB_ERROR;
    }
    pCsr->aOffset[iOut] = iInput;

    if( iInput<nInput ){
      U8_NEXT(zInput, iInput, nInput, c);
    }else{
      c = 0;
    }
  }

  pCsr->pIter = ubrk_open(UBRK_WORD, p->zLocale, pCsr->aChar, iOut, &status);
  if( !U_SUCCESS(status) ){
    capdb_free(pCsr);
    return CAPDB_ERROR;
  }
  pCsr->nChar = iOut;

  ubrk_first(pCsr->pIter);
  *ppCursor = (capdb_tokenizer_cursor *)pCsr;
  return CAPDB_OK;
}

/*
** Close a tokenization cursor previously opened by a call to icuOpen().
*/
static int icuClose(capdb_tokenizer_cursor *pCursor){
  IcuCursor *pCsr = (IcuCursor *)pCursor;
  ubrk_close(pCsr->pIter);
  capdb_free(pCsr->zBuffer);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** Extract the next token from a tokenization cursor.
*/
static int icuNext(
  capdb_tokenizer_cursor *pCursor,  /* Cursor returned by simpleOpen */
  const char **ppToken,               /* OUT: *ppToken is the token text */
  int *pnBytes,                       /* OUT: Number of bytes in token */
  int *piStartOffset,                 /* OUT: Starting offset of token */
  int *piEndOffset,                   /* OUT: Ending offset of token */
  int *piPosition                     /* OUT: Position integer of token */
){
  IcuCursor *pCsr = (IcuCursor *)pCursor;

  int iStart = 0;
  int iEnd = 0;
  int nByte = 0;

  while( iStart==iEnd ){
    UChar32 c;

    iStart = ubrk_current(pCsr->pIter);
    iEnd = ubrk_next(pCsr->pIter);
    if( iEnd==UBRK_DONE ){
      return CAPDB_DONE;
    }

    while( iStart<iEnd ){
      int iWhite = iStart;
      U16_NEXT(pCsr->aChar, iWhite, pCsr->nChar, c);
      if( u_isspace(c) ){
        iStart = iWhite;
      }else{
        break;
      }
    }
    assert(iStart<=iEnd);
  }

  do {
    UErrorCode status = U_ZERO_ERROR;
    if( nByte ){
      char *zNew = capdb_realloc(pCsr->zBuffer, nByte);
      if( !zNew ){
        return CAPDB_NOMEM;
      }
      pCsr->zBuffer = zNew;
      pCsr->nBuffer = nByte;
    }

    u_strToUTF8(
        pCsr->zBuffer, pCsr->nBuffer, &nByte,    /* Output vars */
        &pCsr->aChar[iStart], iEnd-iStart,       /* Input vars */
        &status                                  /* Output success/failure */
    );
  } while( nByte>pCsr->nBuffer );

  *ppToken = pCsr->zBuffer;
  *pnBytes = nByte;
  *piStartOffset = pCsr->aOffset[iStart];
  *piEndOffset = pCsr->aOffset[iEnd];
  *piPosition = pCsr->iToken++;

  return CAPDB_OK;
}

/*
** The set of routines that implement the simple tokenizer
*/
static const capdb_tokenizer_module icuTokenizerModule = {
  0,                           /* iVersion    */
  icuCreate,                   /* xCreate     */
  icuDestroy,                  /* xCreate     */
  icuOpen,                     /* xOpen       */
  icuClose,                    /* xClose      */
  icuNext,                     /* xNext       */
  0,                           /* xLanguageid */
};

/*
** Set *ppModule to point at the implementation of the ICU tokenizer.
*/
void capdbFts3IcuTokenizerModule(
  capdb_tokenizer_module const**ppModule
){
  *ppModule = &icuTokenizerModule;
}

#endif /* defined(CAPDB_ENABLE_ICU) */
#endif /* !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS3) */
