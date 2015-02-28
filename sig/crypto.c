#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/stat.h>

#include "crypto.h"
#include "utils.h"
#include "sphincs256.h"
#include "blake512.h"

#define BUFSIZE 65536

FILE* keyopen(char* prefix, char* postfix) {
  size_t fnlen = strlen(prefix)+strlen(postfix)+1;
  char name[fnlen];
  if( snprintf(name, fnlen, "%s%s", prefix, postfix) != fnlen-1) {
    fprintf(stderr, "couldn't compose filename\n");
    return NULL;
  }
  FILE *fp = fopen(name, "wb");
  if(fp==NULL) {
    fprintf(stderr, "failed to open %s\n", name);
    return NULL;
  }
  return fp;
}

int sig_keyfds(char* name, FILE** key, FILE** pub) {
  if((*pub=keyopen(name, ".pub"))==NULL) return 1;
  if((*key=keyopen(name, ".key"))==NULL) return 1;
  return 0;
}

int sig_genkey(FILE* keyfp, FILE* pubfp) {
  u8 pk[CRYPTO_PUBLICKEYBYTES], sk[CRYPTO_SECRETKEYBYTES];
  if (mlock(pk, CRYPTO_PUBLICKEYBYTES) < 0) {
    fprintf(stderr, "couldn't mlock %d bytes for pubkey.\n", CRYPTO_PUBLICKEYBYTES);
    return -1;
  }
  if (mlock(pk, CRYPTO_SECRETKEYBYTES) < 0) {
    fprintf(stderr, "couldn't mlock %d bytes for secret key.\n", CRYPTO_PUBLICKEYBYTES);
    return -1;
  }

  if(crypto_sign_keypair(pk,sk)!=0) {
    return -1;
  }

  if(fwrite(pk,CRYPTO_PUBLICKEYBYTES,1,pubfp) != 1) {
    fprintf(stderr, "failed to write pubkey: %s\n", strerror(errno));
    return 1;
  }
  fclose(pubfp);
  zerobytes(pk, CRYPTO_PUBLICKEYBYTES);

  fchmod(fileno(keyfp), 0600);
  if(fwrite(sk,CRYPTO_SECRETKEYBYTES,1,keyfp) != 1) {
    fprintf(stderr, "failed to write secret key: %s\n", strerror(errno));
    return 1;
  }
  fclose(keyfp);
  zerobytes(sk, CRYPTO_SECRETKEYBYTES);

  return 0;
}

int sig_sign(void* sk) {
  size_t size;
  unsigned char buf[BUFSIZE];
  blake512_state S;
  blake512_init( &S );
  // buffered hashing and output
  while((size=fread(buf, 1, BUFSIZE, stdin)) > 0) {
    if(!_write(buf, size)) {
      return 1;
    }
    blake512_update( &S, buf, size*8 );
  }

  // calculate sig and output
  unsigned long long hash[8];
  u8 sm[CRYPTO_BYTES+sizeof(hash)];
  unsigned long long smlen;
  blake512_final( &S, (u8*) &hash );
  if(crypto_sign(sm, &smlen,(u8*) &hash, sizeof(hash), sk) == -1) {
    fprintf(stderr, "signing failed\n");
    return 1;
  }
  zerobytes(sk, CRYPTO_SECRETKEYBYTES);

  if(!_write(sm, CRYPTO_BYTES)) {
    return 1;
  }
  return 0;
}

int sig_verify(void* pk) {
  int ret;
  unsigned char buf[BUFSIZE];
  size_t size;
  blake512_state S;
  blake512_init( &S );

  // hash incoming stdin to stdout while always retaining the last
  // CRYPTO_BYTES to be able to use them to verify the message tag
  size=fread(buf, 1, BUFSIZE, stdin);
  while(size > CRYPTO_BYTES) {
    if(!_write(buf, size-CRYPTO_BYTES)) {
      return 1;
    }
    blake512_update( &S, buf, (size-CRYPTO_BYTES)*8 );
    // move last unhashed bytes to  to the beginning of buf
    memcpy(buf, buf+(size-CRYPTO_BYTES), CRYPTO_BYTES);
    if((ret = fread(buf+CRYPTO_BYTES, 1, BUFSIZE-CRYPTO_BYTES, stdin))>0) {
      size=CRYPTO_BYTES+ret;
    } else {
      size=CRYPTO_BYTES;
    }
  }
  fflush(stdout);

  blake512_final( &S, (u8*) buf+CRYPTO_BYTES );
  unsigned long long msg[8];
  unsigned long long msglen;
  if(crypto_sign_open((u8*) &msg, &msglen, buf, CRYPTO_BYTES+sizeof(msg), pk) == -1) {
    fprintf(stderr, "\nverification failed\n");
    return 1;
  }

  zerobytes(pk, CRYPTO_PUBLICKEYBYTES);
  return 0;
}
