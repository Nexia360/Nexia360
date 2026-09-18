/*
** $Id: lundump.c,v 2.7.1.4 2008/04/04 19:51:41 roberto Exp $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#include <string.h>

#define lundump_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"

typedef struct {
 lua_State* L;
 ZIO* Z;
 Mbuffer* b;
 const char* name;
 /* Nexia: the console's chunks are big-endian with 32-bit int and size_t,
 ** and the host is little-endian with a 64-bit size_t. Stock Lua compares the
 ** whole header for equality and rejects them outright, so the header is
 ** parsed instead and these describe what has to be converted while loading.
 ** A chunk built for the host leaves all of them at their neutral values. */
 int swap;
 int size_int;
 int size_size_t;
} LoadState;


#ifdef LUAC_TRUST_BINARIES
#define IF(c,s)
#define error(S,s)
#else
#define IF(c,s)		if (c) error(S,s)

static void error(LoadState* S, const char* why)
{
 luaO_pushfstring(S->L,"%s: %s in precompiled chunk",S->name,why);
 luaD_throw(S->L,LUA_ERRSYNTAX);
}
#endif

#define LoadMem(S,b,n,size)	LoadBlock(S,b,(n)*(size))
#define	LoadByte(S)		(lu_byte)LoadChar(S)
#define LoadVar(S,x)		LoadMem(S,&x,1,sizeof(x))
#define LoadVector(S,b,n,size)	LoadMem(S,b,n,size)

static void LoadBlock(LoadState* S, void* b, size_t size)
{
 size_t r=luaZ_read(S->Z,b,size);
 IF (r!=0, "unexpected end");
}

/* Nexia: reverse one scalar in place. */
static void SwapBytes(void* p, size_t size)
{
 unsigned char* b=(unsigned char*)p;
 size_t i;
 for (i=0; i<size/2; i++)
 {
  unsigned char t=b[i];
  b[i]=b[size-1-i];
  b[size-1-i]=t;
 }
}

/* Nexia: read one integer of the chunk's width and byte order, and return it
** widened to the host's. */
static size_t LoadScalar(LoadState* S, int chunk_size, int is_signed)
{
 unsigned char raw[8];
 size_t value=0;
 int i;
 if (chunk_size<=0 || (size_t)chunk_size>sizeof(raw)) chunk_size=4;
 LoadBlock(S,raw,(size_t)chunk_size);
 if (!S->swap)
  for (i=chunk_size-1; i>=0; i--) value=(value<<8)|raw[i];
 else
  for (i=0; i<chunk_size; i++) value=(value<<8)|raw[i];
 if (is_signed && (size_t)chunk_size<sizeof(size_t))
 {
  const unsigned char top=S->swap ? raw[0] : raw[chunk_size-1];
  if (top&0x80) value|=~(size_t)0<<(chunk_size*8);
 }
 return value;
}

static int LoadChar(LoadState* S)
{
 char x;
 LoadVar(S,x);
 return x;
}

static int LoadInt(LoadState* S)
{
 int x=(int)LoadScalar(S,S->size_int,1);
 IF (x<0, "bad integer");
 return x;
}

static lua_Number LoadNumber(LoadState* S)
{
 lua_Number x;
 LoadVar(S,x);
 if (S->swap) SwapBytes(&x,sizeof(x));
 return x;
}

static TString* LoadString(LoadState* S)
{
 size_t size=LoadScalar(S,S->size_size_t,0);
 if (size==0)
  return NULL;
 else
 {
  char* s=luaZ_openspace(S->L,S->b,size);
  LoadBlock(S,s,size);
  return luaS_newlstr(S->L,s,size-1);		/* remove trailing '\0' */
 }
}

static void LoadCode(LoadState* S, Proto* f)
{
 int n=LoadInt(S);
 f->code=luaM_newvector(S->L,n,Instruction);
 f->sizecode=n;
 LoadVector(S,f->code,n,sizeof(Instruction));
 if (S->swap)
 {
  int i;
  for (i=0; i<n; i++) SwapBytes(&f->code[i],sizeof(Instruction));
 }
}

static Proto* LoadFunction(LoadState* S, TString* p);

static void LoadConstants(LoadState* S, Proto* f)
{
 int i,n;
 n=LoadInt(S);
 f->k=luaM_newvector(S->L,n,TValue);
 f->sizek=n;
 for (i=0; i<n; i++) setnilvalue(&f->k[i]);
 for (i=0; i<n; i++)
 {
  TValue* o=&f->k[i];
  int t=LoadChar(S);
  switch (t)
  {
   case LUA_TNIL:
   	setnilvalue(o);
	break;
   case LUA_TBOOLEAN:
   	setbvalue(o,LoadChar(S)!=0);
	break;
   case LUA_TNUMBER:
	setnvalue(o,LoadNumber(S));
	break;
   case LUA_TSTRING:
	setsvalue2n(S->L,o,LoadString(S));
	break;
   default:
	error(S,"bad constant");
	break;
  }
 }
 n=LoadInt(S);
 f->p=luaM_newvector(S->L,n,Proto*);
 f->sizep=n;
 for (i=0; i<n; i++) f->p[i]=NULL;
 for (i=0; i<n; i++) f->p[i]=LoadFunction(S,f->source);
}

static void LoadDebug(LoadState* S, Proto* f)
{
 int i,n;
 n=LoadInt(S);
 f->lineinfo=luaM_newvector(S->L,n,int);
 f->sizelineinfo=n;
 /* Nexia: line numbers are the chunk's int, not the host's. */
 for (i=0; i<n; i++) f->lineinfo[i]=(int)LoadScalar(S,S->size_int,1);
 n=LoadInt(S);
 f->locvars=luaM_newvector(S->L,n,LocVar);
 f->sizelocvars=n;
 for (i=0; i<n; i++) f->locvars[i].varname=NULL;
 for (i=0; i<n; i++)
 {
  f->locvars[i].varname=LoadString(S);
  f->locvars[i].startpc=LoadInt(S);
  f->locvars[i].endpc=LoadInt(S);
 }
 n=LoadInt(S);
 f->upvalues=luaM_newvector(S->L,n,TString*);
 f->sizeupvalues=n;
 for (i=0; i<n; i++) f->upvalues[i]=NULL;
 for (i=0; i<n; i++) f->upvalues[i]=LoadString(S);
}

static Proto* LoadFunction(LoadState* S, TString* p)
{
 Proto* f;
 if (++S->L->nCcalls > LUAI_MAXCCALLS) error(S,"code too deep");
 f=luaF_newproto(S->L);
 setptvalue2s(S->L,S->L->top,f); incr_top(S->L);
 f->source=LoadString(S); if (f->source==NULL) f->source=p;
 f->linedefined=LoadInt(S);
 f->lastlinedefined=LoadInt(S);
 f->nups=LoadByte(S);
 f->numparams=LoadByte(S);
 f->is_vararg=LoadByte(S);
 f->maxstacksize=LoadByte(S);
 LoadCode(S,f);
 LoadConstants(S,f);
 LoadDebug(S,f);
 IF (!luaG_checkcode(f), "bad code");
 S->L->top--;
 S->L->nCcalls--;
 return f;
}

static void LoadHeader(LoadState* S)
{
 char h[LUAC_HEADERSIZE];
 char s[LUAC_HEADERSIZE];
 int host_little;
 luaU_header(h);
 LoadBlock(S,s,LUAC_HEADERSIZE);
 /* Nexia: signature, version and format still have to match exactly - a
 ** chunk from another Lua is not loadable whatever its byte order. The rest
 ** of the header describes the machine that wrote it, and is converted for
 ** rather than rejected. */
 IF (memcmp(h,s,sizeof(LUA_SIGNATURE)-1+2)!=0, "bad header");
 host_little=h[6];
 S->swap=(s[6]!=host_little);
 S->size_int=s[7];
 S->size_size_t=s[8];
 IF (s[9]!=(char)sizeof(Instruction), "unsupported instruction size");
 IF (s[10]!=(char)sizeof(lua_Number), "unsupported number size");
 IF (s[11]!=h[11], "unsupported number format");
 IF (S->size_int<=0 || S->size_int>(int)sizeof(size_t), "bad int size");
 IF (S->size_size_t<=0 || S->size_size_t>(int)sizeof(size_t),
     "bad size_t size");
}

/*
** load precompiled chunk
*/
Proto* luaU_undump (lua_State* L, ZIO* Z, Mbuffer* buff, const char* name)
{
 LoadState S;
 if (*name=='@' || *name=='=')
  S.name=name+1;
 else if (*name==LUA_SIGNATURE[0])
  S.name="binary string";
 else
  S.name=name;
 S.L=L;
 S.Z=Z;
 S.b=buff;
 S.swap=0;
 S.size_int=(int)sizeof(int);
 S.size_size_t=(int)sizeof(size_t);
 LoadHeader(&S);
 return LoadFunction(&S,luaS_newliteral(L,"=?"));
}

/*
* make header
*/
void luaU_header (char* h)
{
 int x=1;
 memcpy(h,LUA_SIGNATURE,sizeof(LUA_SIGNATURE)-1);
 h+=sizeof(LUA_SIGNATURE)-1;
 *h++=(char)LUAC_VERSION;
 *h++=(char)LUAC_FORMAT;
 *h++=(char)*(char*)&x;				/* endianness */
 *h++=(char)sizeof(int);
 *h++=(char)sizeof(size_t);
 *h++=(char)sizeof(Instruction);
 *h++=(char)sizeof(lua_Number);
 *h++=(char)(((lua_Number)0.5)==0);		/* is lua_Number integral? */
}
