/*
** The "printf" code that follows dates from the 1980's.  It is in
** the public domain. 
**
**************************************************************************
**
** This file contains code for a set of "printf"-like routines.  These
** routines format strings much like the printf() from the standard C
** library, though the implementation here has enhancements to support
** SQLite.
*/
#include "capdbInt.h"

/*
** Conversion types fall into various categories as defined by the
** following enumeration.
*/
#define etRADIX       0 /* non-decimal integer types.  %x %o */
#define etFLOAT       1 /* Floating point.  %f */
#define etEXP         2 /* Exponentional notation. %e and %E */
#define etGENERIC     3 /* Floating or exponential, depending on exponent. %g */
#define etSIZE        4 /* Return number of characters processed so far. %n */
#define etSTRING      5 /* Strings. %s */
#define etDYNSTRING   6 /* Dynamically allocated strings. %z */
#define etPERCENT     7 /* Percent symbol. %% */
#define etCHARX       8 /* Characters. %c */
/* The rest are extensions, not normally found in printf() */
#define etESCAPE_q    9  /* Strings with '\'' doubled.  %q */
#define etESCAPE_Q    10 /* Strings with '\'' doubled and enclosed in '',
                            NULL pointers replaced by SQL NULL.  %Q */
#define etTOKEN       11 /* a pointer to a Token structure */
#define etSRCITEM     12 /* a pointer to a SrcItem */
#define etPOINTER     13 /* The %p conversion */
#define etESCAPE_w    14 /* %w -> Strings with '\"' doubled */
#define etORDINAL     15 /* %r -> 1st, 2nd, 3rd, 4th, etc.  English only */
#define etDECIMAL     16 /* %d or %u, but not %x, %o */
#define etESCAPE_j    17 /* %j -> JSON string literal w/o "..." */
#define etESCAPE_J    18 /* %J -> JSON string literal with "..." */

#define etINVALID     19 /* Any unrecognized conversion type */


/*
** An "etByte" is an 8-bit unsigned value.
*/
typedef unsigned char etByte;

/*
** Each builtin conversion character (ex: the 'd' in "%d") is described
** by an instance of the following structure
*/
typedef struct et_info {   /* Information about each format field */
  char fmttype;            /* The format field code letter */
  etByte base;             /* The base for radix conversion */
  etByte flags;            /* One or more of FLAG_ constants below */
  etByte type;             /* Conversion paradigm */
  etByte charset;          /* Offset into aDigits[] of the digits string */
  etByte prefix;           /* Offset into aPrefix[] of the prefix string */
  char iNxt;               /* Next with same hash, or 0 for end of chain */
} et_info;

/*
** Allowed values for et_info.flags
*/
#define FLAG_SIGNED    1     /* True if the value to convert is signed */
#define FLAG_STRING    4     /* Allow infinite precision */

/*
** The table is searched by hash.  In the case of %C where C is the character
** and that character has ASCII value j, then the hash is j%25.
**
** The order of the entries in fmtinfo[] and the hash chain was entered
** manually, but based on the output of the following TCL script:
*/
#if 0  /*****  Beginning of script ******/
foreach c {d s g z q Q w c o u x X f e E G i n % p T S r J j} {
  scan $c %c x
  set n($c) $x
}
set mx [llength [array names n]]
puts "count: $mx"

set mx 25
puts "*********** mx=$mx ************"
for {set r 0} {$r<$mx} {incr r} {
  puts -nonewline [format %2d: $r]
  foreach c [array names n] {
    if {($n($c))%$mx==$r} {puts -nonewline " $c"}
  }
  puts ""
}
#endif /***** End of script ********/

static const char aDigits[] = "0123456789ABCDEF0123456789abcdef";
static const char aHex[]    = "0123456789abcdef";
static const char aPrefix[] = "-x0\000X0";
static const et_info fmtinfo[25] = {
  /*  0 */  {  'd', 10, 1, etDECIMAL,    0,  0,  0 },
  /*  1 */  {  'e',  0, 1, etEXP,        30, 0,  0 },
  /*  2 */  {  'f',  0, 1, etFLOAT,      0,  0,  0 },
  /*  3 */  {  'g',  0, 1, etGENERIC,    30, 0,  0 },
  /*  4 */  {  'j',  0, 0, etESCAPE_j,   0,  0,  0 },  /* Hash: 6 */
  /*  5 */  {  'i', 10, 1, etDECIMAL,    0,  0,  0 },
  /*  6 */  {  'Q',  0, 4, etESCAPE_Q,   0,  0,  4 },
  /*  7 */  {  'p', 16, 0, etPOINTER,    0,  1,  0 },  /* Hash: 12 */
  /*  8 */  {  'S',  0, 0, etSRCITEM,    0,  0,  0 },
  /*  9 */  {  'T',  0, 0, etTOKEN,      0,  0,  0 },
  /* 10 */  {  'n',  0, 0, etSIZE,       0,  0,  0 },
  /* 11 */  {  'o',  8, 0, etRADIX,      0,  2,  0 },
  /* 12 */  {  '%',  0, 0, etPERCENT,    0,  0,  7 },
  /* 13 */  {  'q',  0, 4, etESCAPE_q,   0,  0, 16 },
  /* 14 */  {  'r', 10, 1, etORDINAL,    0,  0,  0 },
  /* 15 */  {  's',  0, 4, etSTRING,     0,  0,  0 },
  /* 16 */  {  'X', 16, 0, etRADIX,      0,  4,  0 },  /* Hash: 13 */
  /* 17 */  {  'u', 10, 0, etDECIMAL,    0,  0,  0 },
  /* 18 */  {  'w',  0, 4, etESCAPE_w,   0,  0,  0 },  /* Hash: 19 */
  /* 19 */  {  'E',  0, 1, etEXP,        14, 0, 18 },
  /* 20 */  {  'x', 16, 0, etRADIX,      16, 1,  0 },
  /* 21 */  {  'G',  0, 1, etGENERIC,    14, 0,  0 },
  /* 22 */  {  'z',  0, 4, etDYNSTRING,  0,  0,  0 },
  /* 23 */  {  'J',  0, 0, etESCAPE_J,   0,  0,  0 },  /* Hash: 24 */
  /* 24 */  {  'c',  0, 0, etCHARX,      0,  0, 23 }
};

/* Additional Notes:
**
**    %S    Takes a pointer to SrcItem.  Shows name or database.name
**    %!S   Like %S but prefer the zName over the zAlias
*/

/*
** Set the StrAccum object to an error mode.
*/
void capdbStrAccumSetError(StrAccum *p, u8 eError){
  assert( eError==CAPDB_NOMEM || eError==CAPDB_TOOBIG );
  p->accError = eError;
  if( p->mxAlloc ) capdb_str_reset(p);
  if( eError==CAPDB_TOOBIG ) capdbErrorToParser(p->db, eError);
}

/*
** Extra argument values from a PrintfArguments object
*/
static capdb_int64 getIntArg(PrintfArguments *p){
  if( p->nArg<=p->nUsed ) return 0;
  return capdb_value_int64(p->apArg[p->nUsed++]);
}
static double getDoubleArg(PrintfArguments *p){
  if( p->nArg<=p->nUsed ) return 0.0;
  return capdb_value_double(p->apArg[p->nUsed++]);
}
static char *getTextArg(PrintfArguments *p){
  if( p->nArg<=p->nUsed ) return 0;
  return (char*)capdb_value_text(p->apArg[p->nUsed++]);
}

/*
** Allocate memory for a temporary buffer needed for printf rendering.
**
** If the requested size of the temp buffer is larger than the size
** of the output buffer in pAccum, then cause an CAPDB_TOOBIG error.
** Do the size check before the memory allocation to prevent rogue
** SQL from requesting large allocations using the precision or width
** field of the printf() function.
*/
static char *printfTempBuf(capdb_str *pAccum, capdb_int64 n){
  char *z;
  if( pAccum->accError ) return 0;
  if( n>pAccum->nAlloc && n>pAccum->mxAlloc ){
    capdbStrAccumSetError(pAccum, CAPDB_TOOBIG);
    return 0;
  }
  z = capdb_malloc(n);
  if( z==0 ){
    capdbStrAccumSetError(pAccum, CAPDB_NOMEM);
  }
  return z;
}

/*
** On machines with a small stack size, you can redefine the
** CAPDB_PRINT_BUF_SIZE to be something smaller, if desired.
*/
#ifndef CAPDB_PRINT_BUF_SIZE
# define CAPDB_PRINT_BUF_SIZE 70
#endif
#define etBUFSIZE CAPDB_PRINT_BUF_SIZE  /* Size of the output buffer */

/*
** Hard limit on the precision of floating-point conversions.
*/
#ifndef CAPDB_PRINTF_PRECISION_LIMIT
# define CAPDB_FP_PRECISION_LIMIT 100000000
#endif

/*
** Render a string given by "fmt" into the StrAccum object.
*/
void capdb_str_vappendf(
  capdb_str *pAccum,       /* Accumulate results here */
  const char *fmt,           /* Format string */
  va_list ap                 /* arguments */
){
  int c;                     /* Next character in the format string */
  char *bufpt;               /* Pointer to the conversion buffer */
  int precision;             /* Precision of the current field */
  int length;                /* Length of the field */
  int idx;                   /* A general purpose loop counter */
  int width;                 /* Width of the current field */
  etByte flag_leftjustify;   /* True if "-" flag is present */
  etByte flag_prefix;        /* '+' or ' ' or 0 for prefix */
  etByte flag_alternateform; /* True if "#" flag is present */
  etByte flag_altform2;      /* True if "!" flag is present */
  etByte flag_zeropad;       /* True if field width constant starts with zero */
  etByte flag_long;          /* 1 for the "l" flag, 2 for "ll", 0 by default */
  etByte done;               /* Loop termination flag */
  etByte cThousand;          /* Thousands separator for %d and %u */
  etByte xtype = etINVALID;  /* Conversion paradigm */
  u8 bArgList;               /* True for CAPDB_PRINTF_SQLFUNC */
  char prefix;               /* Prefix character.  "+" or "-" or " " or '\0'. */
  sqlite_uint64 longvalue;   /* Value for integer types */
  double realvalue;          /* Value for real types */
  const et_info *infop;      /* Pointer to the appropriate info structure */
  char *zOut;                /* Rendering buffer */
  int nOut;                  /* Size of the rendering buffer */
  char *zExtra = 0;          /* Malloced memory used by some conversion */
  int exp, e2;               /* exponent of real numbers */
  etByte flag_dp;            /* True if decimal point should be shown */
  etByte flag_rtz;           /* True if trailing zeros should be removed */

  PrintfArguments *pArgList = 0; /* Arguments for CAPDB_PRINTF_SQLFUNC */
  char buf[etBUFSIZE];       /* Conversion buffer */

  /* pAccum never starts out with an empty buffer that was obtained from 
  ** malloc().  This precondition is required by the mprintf("%z...")
  ** optimization. */
  assert( pAccum->nChar>0 || (pAccum->printfFlags&CAPDB_PRINTF_MALLOCED)==0 );

  bufpt = 0;
  if( (pAccum->printfFlags & CAPDB_PRINTF_SQLFUNC)!=0 ){
    pArgList = va_arg(ap, PrintfArguments*);
    bArgList = 1;
  }else{
    bArgList = 0;
  }
  for(; (c=(*fmt))!=0; ++fmt){
    if( c!='%' ){
      bufpt = (char *)fmt;
#if HAVE_STRCHRNUL
      fmt = strchrnul(fmt, '%');
#else
      fmt = strchr(fmt, '%');
      if( fmt==0 ){
        fmt = bufpt + strlen(bufpt);
      }
#endif
      capdb_str_append(pAccum, bufpt, (int)(fmt - bufpt));
      if( *fmt==0 ) break;
    }
    if( (c=(*++fmt))==0 ){
      capdb_str_append(pAccum, "%", 1);
      break;
    }
    /* Find out what flags are present */
    flag_leftjustify = flag_prefix = cThousand =
     flag_alternateform = flag_altform2 = flag_zeropad = 0;
    done = 0;
    width = 0;
    flag_long = 0;
    precision = -1;
    do{
      switch( c ){
        case '-':   flag_leftjustify = 1;     break;
        case '+':   flag_prefix = '+';        break;
        case ' ':   flag_prefix = ' ';        break;
        case '#':   flag_alternateform = 1;   break;
        case '!':   flag_altform2 = 1;        break;
        case '0':   flag_zeropad = 1;         break;
        case ',':   cThousand = ',';          break;
        default:    done = 1;                 break;
        case 'l': {
          flag_long = 1;
          c = *++fmt;
          if( c=='l' ){
            c = *++fmt;
            flag_long = 2;
          }
          done = 1;
          break;
        }
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
          unsigned wx = c - '0';
          while( (c = *++fmt)>='0' && c<='9' ){
            wx = wx*10 + c - '0';
          }
          testcase( wx>0x7fffffff );
          width = wx & 0x7fffffff;
#ifdef CAPDB_PRINTF_PRECISION_LIMIT
          if( width>CAPDB_PRINTF_PRECISION_LIMIT ){
            width = CAPDB_PRINTF_PRECISION_LIMIT;
          }
#endif
          if( c!='.' && c!='l' ){
            done = 1;
          }else{
            fmt--;
          }
          break;
        }
        case '*': {
          if( bArgList ){
            width = (int)getIntArg(pArgList);
          }else{
            width = va_arg(ap,int);
          }
          if( width<0 ){
            flag_leftjustify = 1;
            width = width >= -2147483647 ? -width : 0;
          }
#ifdef CAPDB_PRINTF_PRECISION_LIMIT
          if( width>CAPDB_PRINTF_PRECISION_LIMIT ){
            width = CAPDB_PRINTF_PRECISION_LIMIT;
          }
#endif
          if( (c = fmt[1])!='.' && c!='l' ){
            c = *++fmt;
            done = 1;
          }
          break;
        }
        case '.': {
          c = *++fmt;
          if( c=='*' ){
            if( bArgList ){
              precision = (int)getIntArg(pArgList);
            }else{
              precision = va_arg(ap,int);
            }
            if( precision<0 ){
              precision = precision >= -2147483647 ? -precision : -1;
            }
            c = *++fmt;
          }else{
            unsigned px = 0;
            while( c>='0' && c<='9' ){
              px = px*10 + c - '0';
              c = *++fmt;
            }
            testcase( px>0x7fffffff );
            precision = px & 0x7fffffff;
          }
#ifdef CAPDB_PRINTF_PRECISION_LIMIT
          if( precision>CAPDB_PRINTF_PRECISION_LIMIT ){
            precision = CAPDB_PRINTF_PRECISION_LIMIT;
          }
#endif
          if( c=='l' ){
            --fmt;
          }else{
            done = 1;
          }
          break;
        }
      }
    }while( !done && (c=(*++fmt))!=0 );

    /* Fetch the info entry for the field */
#ifdef CAPDB_EBCDIC
    /* The hash table only works for ASCII.  For EBCDIC, we need to do
    ** a linear search of the table */
    infop = &fmtinfo[0];
    xtype = etINVALID;
    for(idx=0; idx<ArraySize(fmtinfo); idx++){
      if( c==fmtinfo[idx].fmttype ){
        infop = &fmtinfo[idx];
        xtype = infop->type;
        break;
      }
    }
#else
    /* Fast hash-table lookup */
    assert( ArraySize(fmtinfo)==25 );
    idx = ((unsigned)c) % 25;
    if( fmtinfo[idx].fmttype==c
     || fmtinfo[idx = fmtinfo[idx].iNxt].fmttype==c
    ){
      infop = &fmtinfo[idx];
      xtype = infop->type;
    }else{
      infop = &fmtinfo[0];
      xtype = etINVALID;
    }
#endif

    /*
    ** At this point, variables are initialized as follows:
    **
    **   flag_alternateform          TRUE if a '#' is present.
    **   flag_altform2               TRUE if a '!' is present.
    **   flag_prefix                 '+' or ' ' or zero
    **   flag_leftjustify            TRUE if a '-' is present or if the
    **                               field width was negative.
    **   flag_zeropad                TRUE if the width began with 0.
    **   flag_long                   1 for "l", 2 for "ll"
    **   width                       The specified field width.  This is
    **                               always non-negative.  Zero is the default.
    **   precision                   The specified precision.  The default
    **                               is -1.
    **   xtype                       The class of the conversion.
    **   infop                       Pointer to the appropriate info struct.
    */
    assert( width>=0 );
    assert( precision>=(-1) );
    switch( xtype ){
      case etPOINTER:
        flag_long = sizeof(char*)==sizeof(i64) ? 2 :
                     sizeof(char*)==sizeof(long int) ? 1 : 0;
        /* no break */ deliberate_fall_through
      case etORDINAL:
      case etRADIX:      
        cThousand = 0;
        /* no break */ deliberate_fall_through
      case etDECIMAL:
        if( infop->flags & FLAG_SIGNED ){
          i64 v;
          if( bArgList ){
            v = getIntArg(pArgList);
          }else if( flag_long ){
            if( flag_long==2 ){
              v = va_arg(ap,i64) ;
            }else{
              v = va_arg(ap,long int);
            }
          }else{
            v = va_arg(ap,int);
          }
          if( v<0 ){
            testcase( v==SMALLEST_INT64 );
            testcase( v==(-1) );
            longvalue = ~v;
            longvalue++;
            prefix = '-';
          }else{
            longvalue = v;
            prefix = flag_prefix;
          }
        }else{
          if( bArgList ){
            longvalue = (u64)getIntArg(pArgList);
          }else if( flag_long ){
            if( flag_long==2 ){
              longvalue = va_arg(ap,u64);
            }else{
              longvalue = va_arg(ap,unsigned long int);
            }
          }else{
            longvalue = va_arg(ap,unsigned int);
          }
          prefix = 0;
        }

#if WHERETRACE_ENABLED
        if( xtype==etPOINTER && capdbWhereTrace & 0x100000 ) longvalue = 0;
#endif
#if TREETRACE_ENABLED
        if( xtype==etPOINTER && capdbTreeTrace & 0x100000 ) longvalue = 0;
#endif

        if( longvalue==0 ) flag_alternateform = 0;
        if( flag_zeropad && precision<width-(prefix!=0) ){
          precision = width-(prefix!=0);
        }
        if( precision<etBUFSIZE-10-etBUFSIZE/3 ){
          nOut = etBUFSIZE;
          zOut = buf;
        }else{
          u64 n;
          n = (u64)precision + 10;
          if( cThousand ) n += precision/3;
          zOut = zExtra = printfTempBuf(pAccum, n);
          if( zOut==0 ) return;
          nOut = (int)n;
        }
        bufpt = &zOut[nOut-1];
        if( xtype==etORDINAL ){
          static const char zOrd[] = "thstndrd";
          int x = (int)(longvalue % 10);
          if( x>=4 || (longvalue/10)%10==1 ){
            x = 0;
          }
          *(--bufpt) = zOrd[x*2+1];
          *(--bufpt) = zOrd[x*2];
        }
        {
          const char *cset = &aDigits[infop->charset];
          u8 base = infop->base;
          do{                                           /* Convert to ascii */
            *(--bufpt) = cset[longvalue%base];
            longvalue = longvalue/base;
          }while( longvalue>0 );
        }
        length = (int)(&zOut[nOut-1]-bufpt);
        if( precision>length ){                         /* zero pad */
          int nn = precision-length;
          bufpt -= nn;
          memset(bufpt,'0',nn);
          length = precision;
        }
        if( cThousand ){
          int nn = (length - 1)/3;  /* Number of "," to insert */
          int ix = (length - 1)%3 + 1;
          bufpt -= nn;
          for(idx=0; nn>0; idx++){
            bufpt[idx] = bufpt[idx+nn];
            ix--;
            if( ix==0 ){
              bufpt[++idx] = cThousand;
              nn--;
              ix = 3;
            }
          }
        }
        if( prefix ) *(--bufpt) = prefix;               /* Add sign */
        if( flag_alternateform && infop->prefix ){      /* Add "0" or "0x" */
          const char *pre;
          char x;
          pre = &aPrefix[infop->prefix];
          for(; (x=(*pre))!=0; pre++) *(--bufpt) = x;
        }
        length = (int)(&zOut[nOut-1]-bufpt);
        break;
      case etFLOAT:
      case etEXP:
      case etGENERIC: {
        FpDecode s;
        int iRound;
        int j;
        i64 szBufNeeded;       /* Size needed to hold the output */

        if( bArgList ){
          realvalue = getDoubleArg(pArgList);
        }else{
          realvalue = va_arg(ap,double);
        }
        if( precision<0 ) precision = 6;         /* Set default precision */
#ifdef CAPDB_FP_PRECISION_LIMIT
        if( precision>CAPDB_FP_PRECISION_LIMIT ){
          precision = CAPDB_FP_PRECISION_LIMIT;
        }
#endif
        if( xtype==etFLOAT ){
          iRound = -precision;
        }else if( xtype==etGENERIC ){
          if( precision==0 ) precision = 1;
          iRound = precision;
        }else{
          iRound = precision+1;
        }
        capdbFpDecode(&s, realvalue, iRound, flag_altform2 ? 20 : 16);
        if( s.isSpecial ){
          if( s.isSpecial==2 ){
            bufpt = flag_zeropad ? "null" : "NaN";
            length = capdbStrlen30(bufpt);
            break;
          }else if( flag_zeropad ){
            s.z[0] = '9';
            s.iDP = 1000;
            s.n = 1;
          }else{
            memcpy(buf, "-Inf", 5);
            bufpt = buf;
            if( s.sign=='-' ){
              /* no-op */
            }else if( flag_prefix ){
              buf[0] = flag_prefix;
            }else{
              bufpt++;
            }
            length = capdbStrlen30(bufpt);
            break;
          }
        }
        if( s.sign=='-' ){
          if( flag_alternateform
           && !flag_prefix
           && xtype==etFLOAT
           && s.iDP<=iRound
          ){
            /* Suppress the minus sign if all of the following are true:
            **   *  The value displayed is zero
            **   *  The '#' flag is used
            **   *  The '+' flag is not used, and
            **   *  The format is %f
            */
            prefix = 0;
          }else{
            prefix = '-';
          }
        }else{
          prefix = flag_prefix;
        }

        exp = s.iDP-1;

        /*
        ** If the field type is etGENERIC, then convert to either etEXP
        ** or etFLOAT, as appropriate.
        */
        if( xtype==etGENERIC ){
          assert( precision>0 );
          precision--;
          flag_rtz = !flag_alternateform;
          if( exp<-4 || exp>precision ){
            xtype = etEXP;
          }else{
            precision = precision - exp;
            xtype = etFLOAT;
          }
        }else{
          flag_rtz = flag_altform2;
        }
        if( xtype==etEXP ){
          e2 = 0;
        }else{
          e2 = s.iDP - 1;
        }

        szBufNeeded = MAX(e2,0)+(i64)precision+(i64)width+10;
        if( cThousand && e2>0 ) szBufNeeded += (e2+2)/3;
        if( szBufNeeded + pAccum->nChar >= pAccum->nAlloc ){
          if( pAccum->mxAlloc==0 && pAccum->accError==0 ){
            /* Unable to allocate space in pAccum, perhaps because it
            ** is coming from capdb_snprintf() or similar.  We'll have
            ** to render into temporary space and the memcpy() it over. */
            bufpt = capdb_malloc(szBufNeeded);
            if( bufpt==0 ){
              capdbStrAccumSetError(pAccum, CAPDB_NOMEM);
              return;
            }
            zExtra = bufpt;
          }else if( capdbStrAccumEnlarge(pAccum, szBufNeeded)<szBufNeeded ){
            width = length = 0;
            break;
          }else{
            bufpt = pAccum->zText + pAccum->nChar;
          }
        }else{
          bufpt = pAccum->zText + pAccum->nChar;
        }
        zOut = bufpt;

        flag_dp = (precision>0 ?1:0) | flag_alternateform | flag_altform2;
        /* The sign in front of the number */
        if( prefix ){
          *(bufpt++) = prefix;
        }
        /* Digits prior to the decimal point */
        j = 0;
        assert( s.n>0 );
        if( e2<0 ){
          *(bufpt++) = '0';
        }else if( cThousand ){
          for(; e2>=0; e2--){
            *(bufpt++) = j<s.n ? s.z[j++] : '0';
            if( (e2%3)==0 && e2>1 ) *(bufpt++) = ',';
          }
        }else{
          j = e2+1;
          if( j>s.n ) j = s.n;
          memcpy(bufpt, s.z, j);
          bufpt += j;
          e2 -= j;
          if( e2>=0 ){
            memset(bufpt, '0', e2+1);
            bufpt += e2+1;
            e2 = -1;
          }
        }
        /* The decimal point */
        if( flag_dp ){
          *(bufpt++) = '.';
        }
        /* "0" digits after the decimal point but before the first
        ** significant digit of the number */
        if( e2<(-1) && precision>0 ){
          int nn = -1-e2;
          if( nn>precision ) nn = precision;
          memset(bufpt, '0', nn);
          bufpt += nn;
          precision -= nn;
        }
        /* Significant digits after the decimal point */
        if( precision>0 ){
          int nn = s.n - j;
          if( NEVER(nn>precision) ) nn = precision;
          if( nn>0 ){
            memcpy(bufpt, s.z+j, nn);
            bufpt += nn;
            precision -= nn;
          }
          if( precision>0 && !flag_rtz ){
            memset(bufpt, '0', precision);
            bufpt += precision;
          }
        }
        /* Remove trailing zeros and the "." if no digits follow the "." */
        if( flag_rtz && flag_dp ){
          while( bufpt[-1]=='0' ) *(--bufpt) = 0;
          assert( bufpt>zOut );
          if( bufpt[-1]=='.' ){
            if( flag_altform2 ){
              *(bufpt++) = '0';
            }else{
              *(--bufpt) = 0;
            }
          }
        }
        /* Add the "eNNN" suffix */
        if( xtype==etEXP ){
          exp = s.iDP - 1;
          *(bufpt++) = aDigits[infop->charset];
          if( exp<0 ){
            *(bufpt++) = '-'; exp = -exp;
          }else{
            *(bufpt++) = '+';
          }
          if( exp>=100 ){
            *(bufpt++) = (char)((exp/100)+'0');        /* 100's digit */
            exp %= 100;
          }
          *(bufpt++) = (char)(exp/10+'0');             /* 10's digit */
          *(bufpt++) = (char)(exp%10+'0');             /* 1's digit */
        }

        length = (int)(bufpt-zOut);
        assert( length <= szBufNeeded );
        if( length<width ){
          i64 nPad = width - length;
          if( flag_leftjustify ){
            memset(bufpt, ' ', nPad);
          }else if( !flag_zeropad ){
            memmove(zOut+nPad, zOut, length);
            memset(zOut, ' ', nPad);
          }else{
            int adj = prefix!=0;
            memmove(zOut+nPad+adj, zOut+adj, length-adj);
            memset(zOut+adj, '0', nPad);
          }
          length = width;
        }

        if( zExtra==0 ){
          /* The result is being rendered directory into pAccum.  This
          ** is the common and fast case */
          pAccum->nChar += length;
          zOut[length] = 0;
          continue;
        }else{
          /* We were unable to render directly into pAccum because we
          ** couldn't allocate sufficient memory.  We need to memcpy()
          ** the rendering (or some prefix thereof) into the output
          ** buffer. */
          bufpt[0] = 0;
          bufpt = zExtra;
          break;
        }
      }
      case etSIZE:
        if( !bArgList ){
          *(va_arg(ap,int*)) = pAccum->nChar;
        }
        length = width = 0;
        break;
      case etPERCENT:
        buf[0] = '%';
        bufpt = buf;
        length = 1;
        break;
      case etCHARX:
        if( bArgList ){
          bufpt = getTextArg(pArgList);
          length = 1;
          if( bufpt ){
            buf[0] = c = *(bufpt++);
            if( (c&0xc0)==0xc0 ){
              while( length<4 && (bufpt[0]&0xc0)==0x80 ){
                buf[length++] = *(bufpt++);
              }
            }
          }else{
            buf[0] = 0;
          }
        }else{
          unsigned int ch = va_arg(ap,unsigned int);
          length = capdbAppendOneUtf8Character(buf, ch);
        }
        if( precision>1 ){
          i64 nPrior = 1;
          width -= precision-1;
          if( width>1 && !flag_leftjustify ){
            capdb_str_appendchar(pAccum, width-1, ' ');
            width = 0;
          }
          capdb_str_append(pAccum, buf, length);
          precision--;
          while( precision > 1 ){
            i64 nCopyBytes;
            if( nPrior > precision-1 ) nPrior = precision - 1;
            nCopyBytes = length*nPrior;
            if( capdbStrAccumEnlargeIfNeeded(pAccum, nCopyBytes) ){
              break;
            }
            capdb_str_append(pAccum,
                 &pAccum->zText[pAccum->nChar-nCopyBytes], nCopyBytes);
            precision -= nPrior;
            nPrior *= 2;
          }
        }
        bufpt = buf;
        flag_altform2 = 1;
        goto adjust_width_for_utf8;
      case etSTRING:
      case etDYNSTRING:
        if( bArgList ){
          bufpt = getTextArg(pArgList);
          xtype = etSTRING;
        }else{
          bufpt = va_arg(ap,char*);
        }
        if( bufpt==0 ){
          bufpt = "";
        }else if( xtype==etDYNSTRING ){
          if( pAccum->nChar==0
           && pAccum->mxAlloc
           && width==0
           && precision<0
           && pAccum->accError==0
          ){
            /* Special optimization for capdb_mprintf("%z..."):
            ** Extend an existing memory allocation rather than creating
            ** a new one. */
            assert( (pAccum->printfFlags&CAPDB_PRINTF_MALLOCED)==0 );
            pAccum->zText = bufpt;
            pAccum->nAlloc = capdbDbMallocSize(pAccum->db, bufpt);
            pAccum->nChar = 0x7fffffff & (int)strlen(bufpt);
            pAccum->printfFlags |= CAPDB_PRINTF_MALLOCED;
            length = 0;
            break;
          }
          zExtra = bufpt;
        }
        if( precision>=0 ){
          if( flag_altform2 ){
            /* Set length to the number of bytes needed in order to display
            ** precision characters */
            unsigned char *z = (unsigned char*)bufpt;
            while( precision-- > 0 && z[0] ){
              CAPDB_SKIP_UTF8(z);
            }
            length = (int)(z - (unsigned char*)bufpt);
          }else{
            for(length=0; length<precision && bufpt[length]; length++){}
          }
        }else{
          length = 0x7fffffff & (int)strlen(bufpt);
        }
      adjust_width_for_utf8:
        if( flag_altform2 && width>0 ){
          /* Adjust width to account for extra bytes in UTF-8 characters */
          int ii = length - 1;
          while( ii>=0 ) if( (bufpt[ii--] & 0xc0)==0x80 ) width++;
        }
        break;
      case etESCAPE_j:           /* %j: JSON string literal w/o "..." */
      case etESCAPE_J: {         /* %J: Generate a JSON string literal */
        char *escarg;
        i64 i, j, px, iStart;
        unsigned char ch;

        if( bArgList ){
          escarg = getTextArg(pArgList);
        }else{
          escarg = va_arg(ap,char*);
        }
        iStart = capdb_str_length(pAccum);
        if( escarg==0 ){
          if( xtype==etESCAPE_J ) capdb_str_append(pAccum, "null", 4);
        }else{
          if( xtype==etESCAPE_J ) capdb_str_append(pAccum, "\"", 1);
          px = precision;
          testcase( px==0 );
          if( px<0 ){
            px = 0x7fffffff;
          }else if( flag_altform2 ){
            /* Convert precision from code-points to bytes */
            for(i=0; i<px && escarg[i]; i++){
              if( (escarg[i]&0xc0)==0x80 ) px++;
            }
            if( i==px ){
              while( (escarg[px]&0xc0)==0x80 ) px++;
            }
          }
          for(i=j=0; i<px; i++){
            if( (ch = ((u8*)escarg)[i])<=0x1f || ch=='"' || ch=='\\' ){
              if( j<i ) capdb_str_append(pAccum, &escarg[j], i-j);
              j = i+1;
              if( ch==0 ) break;
              capdb_str_appendchar(pAccum, 1, '\\');
              if( ch>0x1f ){
                capdb_str_appendchar(pAccum, 1, ch);
              }else if( ((1u<<ch)&0x3700)!=0 ){
                ch = "btn?fr"[ch-8];
                capdb_str_appendchar(pAccum, 1, ch);
              }else{
                capdb_str_append(pAccum, "u00", 3);
                capdb_str_appendchar(pAccum, 1, aHex[ch>>4]);
                capdb_str_appendchar(pAccum, 1, aHex[ch&0xf]);
              }
            }
          }
          if( j<i ) capdb_str_append(pAccum, &escarg[j], i-j);
          if( xtype==etESCAPE_J ) capdb_str_append(pAccum, "\"", 1);
        }
        if( width>0 && capdb_str_errcode(pAccum)==CAPDB_OK ){
          capdb_int64 n = capdb_str_length(pAccum) - iStart;
          capdb_int64 len = n;
          char *zz;
          if( flag_altform2 && n>0 ){
            zz = capdb_str_value(pAccum);
            for(i=iStart; zz[i]; i++){
              if( (zz[i]&0xc0)==0x80 ) len--;
            }
          }
          if( width>len ){
            capdb_int64 sp = width-len;
            assert( sp>0 && sp<0x7fffffff );
            capdb_str_appendchar(pAccum, (int)sp, ' ');
            if( !flag_leftjustify
             && n>0
             && capdb_str_errcode(pAccum)==0
            ){
              zz = capdb_str_value(pAccum);
              zz += iStart;
              memmove(zz+sp, zz, n);
              memset(zz, ' ', sp);
            }
          }
        }
        continue;
      }
      case etESCAPE_q:          /* %q: Escape ' characters */
      case etESCAPE_Q:          /* %Q: Escape ' and enclose in '...' */
      case etESCAPE_w: {        /* %w: Escape " characters */
        i64 i, j, k, n;
        int needQuote = 0;
        char ch;
        char *escarg;
        char q;

        if( bArgList ){
          escarg = getTextArg(pArgList);
        }else{
          escarg = va_arg(ap,char*);
        }
        if( escarg==0 ){
          escarg = (xtype==etESCAPE_Q ? "NULL" : "(NULL)");
        }else if( xtype==etESCAPE_Q ){
          needQuote = 1;
        }
        if( xtype==etESCAPE_w ){
          q = '"';
          flag_alternateform = 0;
        }else{
          q = '\'';
        }
        /* For %q, %Q, and %w, the precision is the number of bytes (or
        ** characters if the ! flags is present) to use from the input.
        ** Because of the extra quoting characters inserted, the number
        ** of output characters may be larger than the precision.
        */
        k = precision;
        for(i=n=0; k!=0 && (ch=escarg[i])!=0; i++, k--){
          if( ch==q )  n++;
          if( flag_altform2 && (ch&0xc0)==0xc0 ){
            while( (escarg[i+1]&0xc0)==0x80 ){ i++; }
          }
        }
        if( flag_alternateform ){
          /* For %#q, do unistr()-style backslash escapes for
          ** all control characters, and for backslash itself.
          ** For %#Q, do the same but only if there is at least
          ** one control character. */
          i64 nBack = 0;
          i64 nCtrl = 0;
          for(k=0; k<i; k++){
            if( escarg[k]=='\\' ){
              nBack++;
            }else if( ((u8*)escarg)[k]<=0x1f ){
              nCtrl++;
            }
          }
          if( nCtrl || xtype==etESCAPE_q ){
            n += nBack + 5*nCtrl;
            if( xtype==etESCAPE_Q ){
              n += 10;
              needQuote = 2;
            }
          }else{
            flag_alternateform = 0;
          }
        }
        n += i + 3;
        if( n>etBUFSIZE ){
          bufpt = zExtra = printfTempBuf(pAccum, n);
          if( bufpt==0 ) return;
        }else{
          bufpt = buf;
        }
        j = 0;
        if( needQuote ){
          if( needQuote==2 ){
            memcpy(&bufpt[j], "unistr('", 8);
            j += 8;
          }else{
            bufpt[j++] = '\'';
          }
        }
        k = i;
        if( flag_alternateform ){
          for(i=0; i<k; i++){
            bufpt[j++] = ch = escarg[i];
            if( ch==q ){
              bufpt[j++] = ch;
            }else if( ch=='\\' ){
              bufpt[j++] = '\\';
            }else if( ((unsigned char)ch)<=0x1f ){
              bufpt[j-1] = '\\';
              bufpt[j++] = 'u';
              bufpt[j++] = '0';
              bufpt[j++] = '0';
              bufpt[j++] = ch>=0x10 ? '1' : '0';
              bufpt[j++] = aHex[ch&0xf];
            }
          }
        }else{
          for(i=0; i<k; i++){
            bufpt[j++] = ch = escarg[i];
            if( ch==q ) bufpt[j++] = ch;
          }
        }
        if( needQuote ){
          bufpt[j++] = '\'';
          if( needQuote==2 ) bufpt[j++] = ')';
        }
        bufpt[j] = 0;
        length = j;
        goto adjust_width_for_utf8;
      }
      case etTOKEN: {
        if( (pAccum->printfFlags & CAPDB_PRINTF_INTERNAL)==0 ) return;
        if( flag_alternateform ){
          /* %#T means an Expr pointer that uses Expr.u.zToken */
          Expr *pExpr = va_arg(ap,Expr*);
          if( ALWAYS(pExpr) && ALWAYS(!ExprHasProperty(pExpr,EP_IntValue)) ){
            capdb_str_appendall(pAccum, (const char*)pExpr->u.zToken);
            capdbRecordErrorOffsetOfExpr(pAccum->db, pExpr);
          }
        }else{
          /* %T means a Token pointer */
          Token *pToken = va_arg(ap, Token*);
          assert( bArgList==0 );
          if( pToken && pToken->n ){
            capdb_str_append(pAccum, (const char*)pToken->z, pToken->n);
            capdbRecordErrorByteOffset(pAccum->db, pToken->z);
          }
        }
        length = width = 0;
        break;
      }
      case etSRCITEM: {
        SrcItem *pItem;
        if( (pAccum->printfFlags & CAPDB_PRINTF_INTERNAL)==0 ) return;
        pItem = va_arg(ap, SrcItem*);
        assert( bArgList==0 );
        if( pItem->zAlias && !flag_altform2 ){
          capdb_str_appendall(pAccum, pItem->zAlias);
        }else if( pItem->zName ){
          if( pItem->fg.fixedSchema==0
           && pItem->fg.isSubquery==0
           && pItem->u4.zDatabase!=0
          ){
            capdb_str_appendall(pAccum, pItem->u4.zDatabase);
            capdb_str_append(pAccum, ".", 1);
          }
          capdb_str_appendall(pAccum, pItem->zName);
        }else if( pItem->zAlias ){
          capdb_str_appendall(pAccum, pItem->zAlias);
        }else if( ALWAYS(pItem->fg.isSubquery) ){/* Because of tag-20240424-1 */
          Select *pSel = pItem->u4.pSubq->pSelect;
          assert( pSel!=0 ); 
          if( pSel->selFlags & SF_NestedFrom ){
            capdb_str_appendf(pAccum, "(join-%u)", pSel->selId);
          }else if( pSel->selFlags & SF_MultiValue ){
            assert( !pItem->fg.isTabFunc && !pItem->fg.isIndexedBy );
            capdb_str_appendf(pAccum, "%u-ROW VALUES CLAUSE",
                                pItem->u1.nRow);
          }else{
            capdb_str_appendf(pAccum, "(subquery-%u)", pSel->selId);
          }
        }
        length = width = 0;
        break;
      }
      default: {
        assert( xtype==etINVALID );
        return;
      }
    }/* End switch over the format type */
    /*
    ** The text of the conversion is pointed to by "bufpt" and is
    ** "length" characters long.  The field width is "width".  Do
    ** the output.  Both length and width are in bytes, not characters,
    ** at this point.  If the "!" flag was present on string conversions
    ** indicating that width and precision should be expressed in characters,
    ** then the values have been translated prior to reaching this point.
    */
    width -= length;
    if( width>0 ){
      if( !flag_leftjustify ) capdb_str_appendchar(pAccum, width, ' ');
      capdb_str_append(pAccum, bufpt, length);
      if( flag_leftjustify ) capdb_str_appendchar(pAccum, width, ' ');
    }else{
      capdb_str_append(pAccum, bufpt, length);
    }

    if( zExtra ){
      capdbDbFree(pAccum->db, zExtra);
      zExtra = 0;
    }
  }/* End for loop over the format string */
} /* End of function */


/*
** The z string points to the first character of a token that is
** associated with an error.  If db does not already have an error
** byte offset recorded, try to compute the error byte offset for
** z and set the error byte offset in db.
*/
void capdbRecordErrorByteOffset(capdb *db, const char *z){
  const Parse *pParse;
  const char *zText;
  const char *zEnd;
  assert( z!=0 );
  if( NEVER(db==0) ) return;
  if( db->errByteOffset!=(-2) ) return;
  pParse = db->pParse;
  if( NEVER(pParse==0) ) return;
  zText =pParse->zTail;
  if( NEVER(zText==0) ) return;
  zEnd = &zText[strlen(zText)];
  if( CAPDB_WITHIN(z,zText,zEnd) ){
    db->errByteOffset = (int)(z-zText);
  }
}

/*
** If pExpr has a byte offset for the start of a token, record that as
** as the error offset.
*/
void capdbRecordErrorOffsetOfExpr(capdb *db, const Expr *pExpr){
  while( pExpr
     && (ExprHasProperty(pExpr,EP_OuterON|EP_InnerON) || pExpr->w.iOfst<=0)
  ){
    pExpr = pExpr->pLeft;
  }
  if( pExpr==0 ) return;
  if( ExprHasProperty(pExpr, EP_FromDDL) ) return;
  db->errByteOffset = pExpr->w.iOfst;
}

/*
** Enlarge the memory allocation on a StrAccum object so that it is
** able to accept at least N more bytes of text.
**
** Return the number of bytes of text that StrAccum is able to accept
** after the attempted enlargement.  The value returned might be zero.
*/
int capdbStrAccumEnlarge(StrAccum *p, i64 N){
  char *zNew;
  assert( p->nChar+N >= p->nAlloc ); /* Only called if really needed */
  if( p->accError ){
    testcase(p->accError==CAPDB_TOOBIG);
    testcase(p->accError==CAPDB_NOMEM);
    return 0;
  }
  if( p->mxAlloc==0 ){
    capdbStrAccumSetError(p, CAPDB_TOOBIG);
    return p->nAlloc - p->nChar - 1;
  }else{
    char *zOld = isMalloced(p) ? p->zText : 0;
    i64 szNew = p->nChar + N + 1;
    if( szNew+p->nChar<=p->mxAlloc ){
      /* Force exponential buffer size growth as long as it does not overflow,
      ** to avoid having to call this routine too often */
      szNew += p->nChar;
    }
    if( szNew > p->mxAlloc ){
      capdb_str_reset(p);
      capdbStrAccumSetError(p, CAPDB_TOOBIG);
      return 0;
    }else{
      p->nAlloc = (int)szNew;
    }
    if( p->db ){
      zNew = capdbDbRealloc(p->db, zOld, p->nAlloc);
    }else{
      zNew = capdbRealloc(zOld, p->nAlloc);
    }
    if( zNew ){
      assert( p->zText!=0 || p->nChar==0 );
      if( !isMalloced(p) && p->nChar>0 ) memcpy(zNew, p->zText, p->nChar);
      p->zText = zNew;
      p->nAlloc = capdbDbMallocSize(p->db, zNew);
      p->printfFlags |= CAPDB_PRINTF_MALLOCED;
    }else{
      capdb_str_reset(p);
      capdbStrAccumSetError(p, CAPDB_NOMEM);
      return 0;
    }
  }
  assert( N>=0 && N<=0x7fffffff );
  return (int)N;
}

int capdbStrAccumEnlargeIfNeeded(StrAccum *p, i64 N){
  if( N + p->nChar >= p->nAlloc ){
    capdbStrAccumEnlarge(p, N);
  }
  return p->accError;
}

/*
** Append N copies of character c to the given string buffer.
*/
void capdb_str_appendchar(capdb_str *p, int N, char c){
  testcase( p->nChar + (i64)N > 0x7fffffff );
  if( p->nChar+(i64)N >= p->nAlloc && (N = capdbStrAccumEnlarge(p, N))<=0 ){
    return;
  }
  while( (N--)>0 ) p->zText[p->nChar++] = c;
}

/*
** The StrAccum "p" is not large enough to accept N new bytes of z[].
** So enlarge if first, then do the append.
**
** This is a helper routine to capdb_str_append() that does special-case
** work (enlarging the buffer) using tail recursion, so that the
** capdb_str_append() routine can use fast calling semantics.
*/
static void CAPDB_NOINLINE enlargeAndAppend(StrAccum *p, const char *z, int N){
  N = capdbStrAccumEnlarge(p, N);
  if( N>0 ){
    memcpy(&p->zText[p->nChar], z, N);
    p->nChar += N;
  }
}

/*
** Append N bytes of text from z to the StrAccum object.  Increase the
** size of the memory allocation for StrAccum if necessary.
*/
void capdb_str_append(capdb_str *p, const char *z, int N){
  assert( z!=0 || N==0 );
  assert( p->zText!=0 || p->nChar==0 || p->accError );
  assert( N>=0 );
  assert( p->accError==0 || p->nAlloc==0 || p->mxAlloc==0 );
  if( p->nChar+N >= p->nAlloc ){
    enlargeAndAppend(p,z,N);
  }else if( N ){
    assert( p->zText );
    p->nChar += N;
    memcpy(&p->zText[p->nChar-N], z, N);
  }
}

/*
** Append the complete text of zero-terminated string z[] to the p string.
*/
void capdb_str_appendall(capdb_str *p, const char *z){
  capdb_str_append(p, z, capdbStrlen30(z));
}


/*
** Finish off a string by making sure it is zero-terminated.
** Return a pointer to the resulting string.  Return a NULL
** pointer if any kind of error was encountered.
*/
static CAPDB_NOINLINE char *strAccumFinishRealloc(StrAccum *p){
  char *zText;
  assert( p->mxAlloc>0 && !isMalloced(p) );
  zText = capdbDbMallocRaw(p->db, 1+(u64)p->nChar );
  if( zText ){
    memcpy(zText, p->zText, p->nChar+1);
    p->printfFlags |= CAPDB_PRINTF_MALLOCED;
  }else{
    capdbStrAccumSetError(p, CAPDB_NOMEM);
  }
  p->zText = zText;
  return zText;
}
char *capdbStrAccumFinish(StrAccum *p){
  if( p->zText ){
    p->zText[p->nChar] = 0;
    if( p->mxAlloc>0 && !isMalloced(p) ){
      return strAccumFinishRealloc(p);
    }
  }
  return p->zText;
}

/*
** Use the content of the StrAccum passed as the second argument
** as the result of an SQL function.
*/
void capdbResultStrAccum(capdb_context *pCtx, StrAccum *p){
  if( p->accError ){
    capdb_result_error_code(pCtx, p->accError);
    capdb_str_reset(p);
  }else if( isMalloced(p) ){
    capdb_result_text(pCtx, p->zText, p->nChar, CAPDB_DYNAMIC);
  }else{
    capdb_result_text(pCtx, "", 0, CAPDB_STATIC);
    capdb_str_reset(p);
  }
}

/*
** This singleton is an capdb_str object that is returned if
** capdb_malloc() fails to provide space for a real one.  This
** capdb_str object accepts no new text and always returns
** an CAPDB_NOMEM error.
*/
static capdb_str capdbOomStr = {
   0, 0, 0, 0, 0, CAPDB_NOMEM, 0
};

/* Finalize a string created using capdb_str_new().
*/
char *capdb_str_finish(capdb_str *p){
  char *z;
  if( p!=0 && p!=&capdbOomStr ){
    z = capdbStrAccumFinish(p);
    capdb_free(p);
  }else{
    z = 0;
  }
  return z;
}

/* Return any error code associated with p */
int capdb_str_errcode(capdb_str *p){
  return p ? p->accError : CAPDB_NOMEM;
}

/* Return the current length of p in bytes */
int capdb_str_length(capdb_str *p){
  return p ? p->nChar : 0;
}

/* Truncate the text of the string to be no more than N bytes. */
void capdb_str_truncate(capdb_str *p, int N){
  if( p!=0 && N>=0 && (u32)N<p->nChar ){
    p->nChar = N;
    p->zText[p->nChar] = 0;
  }
}

/* Return the current value for p */
char *capdb_str_value(capdb_str *p){
  if( p==0 || p->nChar==0 ) return 0;
  p->zText[p->nChar] = 0;
  return p->zText;
}

/*
** Reset an StrAccum string.  Reclaim all malloced memory.
*/
void capdb_str_reset(StrAccum *p){
  if( isMalloced(p) ){
    capdbDbFree(p->db, p->zText);
    p->printfFlags &= ~CAPDB_PRINTF_MALLOCED;
  }
  p->nAlloc = 0;
  p->nChar = 0;
  p->zText = 0;
}

/*
** Destroy a dynamically allocate capdb_str object and all
** of its content, all in one call.
*/
void capdb_str_free(capdb_str *p){
  if( p!=0 && p!=&capdbOomStr ){
    capdb_str_reset(p);
    capdb_free(p);
  }
}

/*
** Initialize a string accumulator.
**
** p:     The accumulator to be initialized.
** db:    Pointer to a database connection.  May be NULL.  Lookaside
**        memory is used if not NULL. db->mallocFailed is set appropriately
**        when not NULL.
** zBase: An initial buffer.  May be NULL in which case the initial buffer
**        is malloced.
** n:     Size of zBase in bytes.  If total space requirements never exceed
**        n then no memory allocations ever occur.
** mx:    Maximum number of bytes to accumulate.  If mx==0 then no memory
**        allocations will ever occur.
*/
void capdbStrAccumInit(StrAccum *p, capdb *db, char *zBase, int n, int mx){
  p->zText = zBase;
  p->db = db;
  p->nAlloc = n;
  p->mxAlloc = mx;
  p->nChar = 0;
  p->accError = 0;
  p->printfFlags = 0;
}

/* Allocate and initialize a new dynamic string object */
capdb_str *capdb_str_new(capdb *db){
  capdb_str *p = capdb_malloc64(sizeof(*p));
  if( p ){
    capdbStrAccumInit(p, 0, 0, 0,
            db ? db->aLimit[CAPDB_LIMIT_LENGTH] : CAPDB_MAX_LENGTH);
  }else{
    p = &capdbOomStr;
  }
  return p;
}

/*
** Print into memory obtained from sqliteMalloc().  Use the internal
** %-conversion extensions.
*/
char *capdbVMPrintf(capdb *db, const char *zFormat, va_list ap){
  char *z;
  char zBase[CAPDB_PRINT_BUF_SIZE];
  StrAccum acc;
  assert( db!=0 );
  capdbStrAccumInit(&acc, db, zBase, sizeof(zBase),
                      db->aLimit[CAPDB_LIMIT_LENGTH]);
  acc.printfFlags = CAPDB_PRINTF_INTERNAL;
  capdb_str_vappendf(&acc, zFormat, ap);
  z = capdbStrAccumFinish(&acc);
  if( acc.accError==CAPDB_NOMEM ){
    capdbOomFault(db);
  }
  return z;
}

/*
** Print into memory obtained from sqliteMalloc().  Use the internal
** %-conversion extensions.
*/
char *capdbMPrintf(capdb *db, const char *zFormat, ...){
  va_list ap;
  char *z;
  va_start(ap, zFormat);
  z = capdbVMPrintf(db, zFormat, ap);
  va_end(ap);
  return z;
}

/*
** Print into memory obtained from capdb_malloc().  Omit the internal
** %-conversion extensions.
*/
char *capdb_vmprintf(const char *zFormat, va_list ap){
  char *z;
  char zBase[CAPDB_PRINT_BUF_SIZE];
  StrAccum acc;

#ifdef CAPDB_ENABLE_API_ARMOR  
  if( zFormat==0 ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  capdbStrAccumInit(&acc, 0, zBase, sizeof(zBase), CAPDB_MAX_LENGTH);
  capdb_str_vappendf(&acc, zFormat, ap);
  z = capdbStrAccumFinish(&acc);
  return z;
}

/*
** Print into memory obtained from capdb_malloc()().  Omit the internal
** %-conversion extensions.
*/
char *capdb_mprintf(const char *zFormat, ...){
  va_list ap;
  char *z;
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  va_start(ap, zFormat);
  z = capdb_vmprintf(zFormat, ap);
  va_end(ap);
  return z;
}

/*
** capdb_snprintf() works like snprintf() except that it ignores the
** current locale settings.  This is important for SQLite because we
** are not able to use a "," as the decimal point in place of "." as
** specified by some locales.
**
** Oops:  The first two arguments of capdb_snprintf() are backwards
** from the snprintf() standard.  Unfortunately, it is too late to change
** this without breaking compatibility, so we just have to live with the
** mistake.
**
** capdb_vsnprintf() is the varargs version.
*/
char *capdb_vsnprintf(int n, char *zBuf, const char *zFormat, va_list ap){
  StrAccum acc;
  if( n<=0 ) return zBuf;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( zBuf==0 || zFormat==0 ) {
    (void)CAPDB_MISUSE_BKPT;
    if( zBuf ) zBuf[0] = 0;
    return zBuf;
  }
#endif
  capdbStrAccumInit(&acc, 0, zBuf, n, 0);
  capdb_str_vappendf(&acc, zFormat, ap);
  zBuf[acc.nChar] = 0;
  return zBuf;
}
char *capdb_snprintf(int n, char *zBuf, const char *zFormat, ...){
  StrAccum acc;
  va_list ap;
  if( n<=0 ) return zBuf;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( zBuf==0 || zFormat==0 ) {
    (void)CAPDB_MISUSE_BKPT;
    if( zBuf ) zBuf[0] = 0;
    return zBuf;
  }
#endif
  capdbStrAccumInit(&acc, 0, zBuf, n, 0);
  va_start(ap,zFormat);
  capdb_str_vappendf(&acc, zFormat, ap);
  va_end(ap);
  zBuf[acc.nChar] = 0;
  return zBuf;
}

/* Maximum size of an capdb_log() message. */
#if defined(CAPDB_MAX_LOG_MESSAGE) 
  /* Leave the definition as supplied */
#elif CAPDB_PRINT_BUF_SIZE*10>10000
# define CAPDB_MAX_LOG_MESSAGE 10000
#else
# define CAPDB_MAX_LOG_MESSAGE (CAPDB_PRINT_BUF_SIZE*10)
#endif

/*
** This is the routine that actually formats the capdb_log() message.
** We house it in a separate routine from capdb_log() to avoid using
** stack space on small-stack systems when logging is disabled.
**
** capdb_log() must render into a static buffer.  It cannot dynamically
** allocate memory because it might be called while the memory allocator
** mutex is held.
**
** capdb_str_vappendf() might ask for *temporary* memory allocations for
** certain format characters (%q) or for very large precisions or widths.
** Care must be taken that any capdb_log() calls that occur while the
** memory mutex is held do not use these mechanisms.
*/
static void renderLogMsg(int iErrCode, const char *zFormat, va_list ap){
  StrAccum acc;                          /* String accumulator */
  char zMsg[CAPDB_MAX_LOG_MESSAGE];     /* Complete log message */

  capdbStrAccumInit(&acc, 0, zMsg, sizeof(zMsg), 0);
  capdb_str_vappendf(&acc, zFormat, ap);
  capdbGlobalConfig.xLog(capdbGlobalConfig.pLogArg, iErrCode,
                           capdbStrAccumFinish(&acc));
}

/*
** Format and write a message to the log if logging is enabled.
*/
void capdb_log(int iErrCode, const char *zFormat, ...){
  va_list ap;                             /* Vararg list */
  if( capdbGlobalConfig.xLog ){
    va_start(ap, zFormat);
    renderLogMsg(iErrCode, zFormat, ap);
    va_end(ap);
  }
}

#if defined(CAPDB_DEBUG) || defined(CAPDB_HAVE_OS_TRACE)
/*
** A version of printf() that understands %lld.  Used for debugging.
** The printf() built into some versions of windows does not understand %lld
** and segfaults if you give it a long long int.
*/
void capdbDebugPrintf(const char *zFormat, ...){
  va_list ap;
  StrAccum acc;
  char zBuf[CAPDB_PRINT_BUF_SIZE*10];
  capdbStrAccumInit(&acc, 0, zBuf, sizeof(zBuf), 0);
  va_start(ap,zFormat);
  capdb_str_vappendf(&acc, zFormat, ap);
  va_end(ap);
  capdbStrAccumFinish(&acc);
#ifdef CAPDB_OS_TRACE_PROC
  {
    extern void CAPDB_OS_TRACE_PROC(const char *zBuf, int nBuf);
    CAPDB_OS_TRACE_PROC(zBuf, sizeof(zBuf));
  }
#else
  fprintf(stdout,"%s", zBuf);
  fflush(stdout);
#endif
}
#endif


/*
** variable-argument wrapper around capdb_str_vappendf(). The bFlags argument
** can contain the bit CAPDB_PRINTF_INTERNAL enable internal formats.
*/
void capdb_str_appendf(StrAccum *p, const char *zFormat, ...){
  va_list ap;
  va_start(ap,zFormat);
  capdb_str_vappendf(p, zFormat, ap);
  va_end(ap);
}


/*****************************************************************************
** Reference counted string/blob storage
*****************************************************************************/

/*
** Increase the reference count of the string by one.
**
** The input parameter is returned.
*/
char *capdbRCStrRef(char *z){
  RCStr *p = (RCStr*)z;
  assert( p!=0 );
  p--;
  p->nRCRef++;
  return z;
}

/*
** Decrease the reference count by one.  Free the string when the
** reference count reaches zero.
*/
void capdbRCStrUnref(void *z){
  RCStr *p = (RCStr*)z;
  assert( p!=0 );
  p--;
  assert( p->nRCRef>0 );
  if( p->nRCRef>=2 ){
    p->nRCRef--;
  }else{
    capdb_free(p);
  }
}

/*
** Create a new string that is capable of holding N bytes of text, not counting
** the zero byte at the end.  The string is uninitialized.
**
** The reference count is initially 1.  Call capdbRCStrUnref() to free the
** newly allocated string.
**
** This routine returns 0 on an OOM.
*/
char *capdbRCStrNew(u64 N){
  RCStr *p = capdb_malloc64( N + sizeof(*p) + 1 );
  if( p==0 ) return 0;
  p->nRCRef = 1;
  return (char*)&p[1];
}

/*
** Change the size of the string so that it is able to hold N bytes.
** The string might be reallocated, so return the new allocation.
*/
char *capdbRCStrResize(char *z, u64 N){
  RCStr *p = (RCStr*)z;
  RCStr *pNew;
  assert( p!=0 );
  p--;
  assert( p->nRCRef==1 );
  pNew = capdb_realloc64(p, N+sizeof(RCStr)+1);
  if( pNew==0 ){
    capdb_free(p);
    return 0;
  }else{
    return (char*)&pNew[1];
  }
}
