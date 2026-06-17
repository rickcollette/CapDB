/*
** JNI bindings for capdb remote SQL connections.
*/
#if defined(CAPDB_ENABLE_NETWORK)

#include "../client/capdb_client.h"
#include "capdb.h"
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

JNIEXPORT jlong JNICALL Java_org_sqlite_jni_wrapper1_Sqlite_connectRemoteNative(
  JNIEnv *env, jclass cls, jstring jUri
){
  const char *zUri;
  capdb_conn *p = 0;
  int rc;
  (void)cls;
  zUri = (*env)->GetStringUTFChars(env, jUri, 0);
  if( zUri==0 ) return 0;
  rc = capdb_net_connect(zUri, &p);
  (*env)->ReleaseStringUTFChars(env, jUri, zUri);
  if( rc!=CAPDB_NET_OK ){
    const char *zMsg = capdb_net_errmsg(p);
    if( zMsg==0 || zMsg[0]==0 ) zMsg = "capdb remote connect failed";
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/sql/SQLException"), zMsg);
    return 0;
  }
  return (jlong)(intptr_t)p;
}

JNIEXPORT void JNICALL Java_org_sqlite_jni_wrapper1_Sqlite_closeRemoteNative(
  JNIEnv *env, jclass cls, jlong ptr
){
  capdb_conn *p = (capdb_conn*)(intptr_t)ptr;
  (void)env; (void)cls;
  capdb_net_close(p);
}

JNIEXPORT jint JNICALL Java_org_sqlite_jni_wrapper1_Sqlite_execRemoteNative(
  JNIEnv *env, jclass cls, jlong ptr, jstring jSql
){
  capdb_conn *p = (capdb_conn*)(intptr_t)ptr;
  const char *zSql;
  int rc;
  (void)cls;
  if( p==0 ) return CAPDB_MISUSE;
  zSql = (*env)->GetStringUTFChars(env, jSql, 0);
  if( zSql==0 ) return CAPDB_ERROR;
  rc = capdb_net_exec(p, zSql, 0, 0);
  (*env)->ReleaseStringUTFChars(env, jSql, zSql);
  if( rc!=CAPDB_NET_OK && p ){
    const char *zMsg = capdb_net_errmsg(p);
    if( zMsg && zMsg[0] ){
      (*env)->ThrowNew(env, (*env)->FindClass(env, "java/sql/SQLException"), zMsg);
    }
  }
  return rc;
}

#endif /* CAPDB_ENABLE_NETWORK */
