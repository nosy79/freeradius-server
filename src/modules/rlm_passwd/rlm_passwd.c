#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/*#include "autoconf.h"
#include "libradius.h"*/
#include "radiusd.h"
#include "modules.h"


struct mypasswd {
	struct mypasswd *next;
	char *field[0];
	char data[1];
};

struct hashtable {
	int tablesize;
	int keyfield;
	int nfields;
	int islist;
	int ignorenis;
	char * filename;
	struct mypasswd **table;
	char buffer[1024];
	FILE *fp;
	char *delimiter;
};


#ifdef TEST
void printpw(struct mypasswd *pw, int nfields){
  int i;
  if (pw) {
  	for( i = 0; i < nfields; i++ ) printf("%s:", pw->field[i]);
  	printf("\n");
  }
  else printf ("Not found\n");
  fflush(stdout);
}
#endif

static int string_to_entry(const char* string, int nfields, char delimiter,
	struct mypasswd *passwd, int bufferlen)
{
	char *str;
	int len, i, fn=0;
	

	len = strlen(string);
	if(!len) return 0;
	if (string[len-1] == '\n') len--;
	if(!len) return 0;
	if (string[len-1] == '\r') len--;
	if(!len) return 0;
	if (!len || !passwd || bufferlen < (len + nfields * sizeof (char*) + sizeof (struct mypasswd) + 1) ) return 0;
	passwd->next = 0;
	str = passwd->data + nfields * sizeof (char *);
	memcpy (str, string, len);
	str[len] = 0;
	passwd->field[fn++] = str;
	for(i=0; i < len; i++){
		if (str[i] == delimiter) {
			str[i] = 0;
			passwd->field[fn++] = str + i + 1;
			if (fn == nfields) break;
		}
	}
	for (; fn < nfields; fn++) passwd->field[fn] = NULL;
/*
printpw(passwd, 7);
*/
	return len + nfields * sizeof (char*) + sizeof (struct mypasswd) + 1;
}


static void destroy_password (struct mypasswd * pass)
{
	if(!pass) return;
	if(pass->next)destroy_password(pass->next);
	free(pass);
}


static unsigned int hash(const unsigned char * username, unsigned int tablesize)
{
	int h=1;
	while (*username) {
		h = h * 7907 + *username++;
	}
	return h%tablesize;
} 

static void release_hash_table(struct hashtable * ht){
	int i;

	if (!ht) return;
	for (i=0; i<ht->tablesize; i++) 
 		if (ht->table[i])
 			destroy_password(ht->table[i]);
	if (ht->table) free(ht->table);
	if (ht->filename) free(ht->filename);
	if (ht->fp) fclose(ht->fp);
	free(ht);
}

static struct hashtable * build_hash_table (const char * file, int nfields,
	int keyfield, int islist, int tablesize, int ignorenis, char * delimiter)
{
#define passwd ((struct mypasswd *) ht->buffer)
	char buffer[1024];
	struct hashtable* ht;
	int len;
	int h;
	struct mypasswd *hashentry, *hashentry1;
	char *list;
	char *nextlist=0;
	int i;
	
	ht = (struct hashtable *) malloc(sizeof(struct hashtable));
	if(!ht) {
		return NULL;
	}
	ht->filename = strdup(file);
	if(!ht->filename) {
		free(ht);
		return NULL;
	}
	ht->tablesize = tablesize;
	ht->nfields = nfields;
	ht->keyfield = keyfield;
	ht->islist = islist;
	ht->ignorenis = ignorenis;
	if (delimiter && *delimiter) ht->delimiter = delimiter;
	else ht->delimiter = ":";
	if(!tablesize) return ht;
	if(!(ht->fp = fopen(file,"r"))) return NULL;
	memset(ht->buffer, 0, 1024);
	ht->table = (struct mypasswd **) malloc (tablesize * sizeof(struct mypasswd *));
	if (!ht->table) {
		/*
		 * Unable allocate memory for hash table
		 * Still work without it
		 */
		ht->tablesize = 0;
		return ht;
	}
	while (fgets(buffer, 1024, ht->fp)) {
		if(*buffer && *buffer!='\n' && (!ignorenis || (*buffer != '+' && *buffer != '-')) ){
			len = strlen(buffer) + nfields * sizeof (char *) + sizeof (struct mypasswd) + 1;
			if(!(hashentry = (struct mypasswd *) malloc(len))){
				release_hash_table(ht);
				ht->tablesize = 0;
				return ht;
			}
			len = string_to_entry(buffer, nfields, *ht->delimiter, hashentry, len);
			if(!hashentry->field[keyfield] || *hashentry->field[keyfield] == '\0') {
				free(hashentry);
				continue;
			}
			
			if (islist) {
				list = hashentry->field[keyfield];
				for (nextlist = list; *nextlist && *nextlist!=','; nextlist++);
				if (*nextlist) *nextlist++ = 0;
				else nextlist = 0;
			}
			h = hash(hashentry->field[keyfield], tablesize);
			hashentry->next = ht->table[h];
			ht->table[h] = hashentry;
			if (islist) {
				for(list=nextlist; nextlist; list = nextlist){
					for (nextlist = list; *nextlist && *nextlist!=','; nextlist++);
					if (*nextlist) *nextlist++ = 0;
					else nextlist = 0;
					if(!(hashentry1 = (struct mypasswd *) malloc(sizeof(struct mypasswd) + nfields * sizeof(char *)))){
						release_hash_table(ht);
						ht->tablesize = 0;
						return ht;
					}
					for (i=0; i<nfields; i++) hashentry1->field[i] = hashentry->field[i];
					hashentry1->field[keyfield] = list;
					h = hash(list, tablesize);
					hashentry1->next = ht->table[h];
					ht->table[h] = hashentry1;
				}
			}
		}
	}
	fclose(ht->fp);
	ht->fp = NULL;
	return ht;
#undef passwd
}

static struct mypasswd * get_next(char *name, struct hashtable *ht)
{
#define passwd ((struct mypasswd *) ht->buffer)
	struct mypasswd * hashentry;
	char buffer[1024];
	int len;
	char *list, *nextlist;
	
	if (ht->tablesize > 0) {
		/* get saved address of next item to check from buffer */
		memcpy(&hashentry, ht->buffer, sizeof(hashentry));
		for (; hashentry; hashentry = hashentry->next) {
			if (!strcmp(hashentry->field[ht->keyfield], name)) {
				/* save new address */
				memcpy(ht->buffer, &hashentry->next, sizeof(hashentry));
				return hashentry;
			}
		}
		memset(ht->buffer, 0, sizeof(hashentry));
		return NULL;
	}
	if (!ht->fp) return NULL;
	while (fgets(buffer, 1024,ht->fp)) {
		if(*buffer && *buffer!='\n' && (len = string_to_entry(buffer, ht->nfields, *ht->delimiter, passwd, 1024)) &&
			(!ht->ignorenis || (*buffer !='-' && *buffer != '+') ) ){
			if(!ht->islist) {
				if(!strcmp(passwd->field[ht->keyfield], name))
					return passwd;
			}
			else {
				for (list = passwd->field[ht->keyfield], nextlist = list; nextlist; list = nextlist) {
					for(nextlist = list; *nextlist && *nextlist!=','; nextlist++);
					if(!*nextlist)nextlist = 0;
					else *nextlist++ = 0;
					if(!strcmp(list, name)) return passwd;
				}
			}
			
		}
	}
	fclose(ht->fp);
	ht->fp = NULL;
	return NULL;
#undef passwd
}

static struct mypasswd * get_pw_nam(char * name, struct hashtable* ht)
{
	int h;
	struct mypasswd * hashentry;
	
	if (!ht || !name || *name == '\0') return NULL;
	if (ht->tablesize > 0) {
		h = hash (name, ht->tablesize);
		for (hashentry = ht->table[h]; hashentry; hashentry = hashentry->next)
			if (!strcmp(hashentry->field[ht->keyfield], name)){
				/* save address of next item to check into buffer */
				memcpy(ht->buffer, &hashentry->next, sizeof(hashentry));
				return hashentry;
			}
		memset(ht->buffer, 0, sizeof(hashentry));
		return NULL;
	}
	if (ht->fp) fclose(ht->fp);
	if (!(ht->fp=fopen(ht->filename, "r"))) return NULL;
	return get_next(name, ht);
}

#ifdef TEST

int main(void){
 struct hashtable *ht;
 char *buffer;
 struct mypasswd* pw;
 int i;
 
 ht = build_hash_table("/etc/group", 4, 3, 1, 100, 0);
 if(!ht) {
 	printf("Hash table not built\n");
 	return -1;
 }
 
 for (i=0; i<ht->tablesize; i++) if (ht->table[i]) {
  printf("%d:\n", i);
  for(pw=ht->table[i]; pw; pw=pw->next) printpw(pw, 4);
 }
 while(fgets(buffer, 1024, stdin)){
  buffer[strlen(buffer)-1] = 0;
  pw = get_pw_nam(buffer, ht);
  printpw(pw,4);
  while (pw = get_next(buffer, ht))printpw(pw,4);
 }
}

#else  /* TEST */
struct passwd_instance {
	struct hashtable *ht;
	struct mypasswd *pwdfmt;
	char *filename;
	char *format;
	char *authtype;
	char * delimiter;
	int allowmultiple;
	int ignorenislike;
	int hashsize;
	int nfields;
	int keyfield;
	int listable;
	int keyattr;
	int keyattrtype;
};

static CONF_PARSER module_config[] = {
	{ "filename",   PW_TYPE_STRING_PTR,
	   offsetof(struct passwd_instance, filename), NULL,  NULL },	
	{ "format",   PW_TYPE_STRING_PTR,
	   offsetof(struct passwd_instance, format), NULL,  NULL },
	{ "authtype",   PW_TYPE_STRING_PTR,
	   offsetof(struct passwd_instance, authtype), NULL,  NULL },
	{ "delimiter",   PW_TYPE_STRING_PTR,
	   offsetof(struct passwd_instance, delimiter), NULL,  ":" },
	{ "ignorenislike",   PW_TYPE_BOOLEAN,
	   offsetof(struct passwd_instance, ignorenislike), NULL,  "yes" },
	{ "allowmultiplekeys",   PW_TYPE_BOOLEAN,
	   offsetof(struct passwd_instance, ignorenislike), NULL,  "no" },
	{ "hashsize",   PW_TYPE_INTEGER,
	   offsetof(struct passwd_instance, hashsize), NULL,  "100" },
};

static int passwd_instantiate(CONF_SECTION *conf, void **instance)
{
#define inst ((struct passwd_instance *)*instance)
	int nfields=0, keyfield=-1, listable=0;
	char *s;
	int len;
	DICT_ATTR * da;
	
	*instance = rad_malloc(sizeof(struct passwd_instance));
	if (cf_section_parse(conf, inst, module_config) < 0) {
		free(inst);
		radlog(L_ERR, "rlm_passwd: cann't parse configuration");
		return -1;
	}
	if(!inst->filename || *inst->filename == '\0' || !inst->format || *inst->format == '\0') {
		radlog(L_ERR, "rlm_passwd: cann't find passwd file and/or format in configuration");
		return -1;
	}
	s = inst->format - 1;
	do {
		if(s == inst->format - 1 || *s == ':'){
			if(*(s+1) == '*'){
				keyfield = nfields;
				if(*(s+2) == ','){
					listable = 1;
				}
			}
			nfields++;
		}
		s++;
	}while(*s);
	if(keyfield < 0) {
		radlog(L_ERR, "rlm_passwd: no field market as key in format: %s", inst->format);
		return -1;
	}
	if (! (inst->ht = build_hash_table (inst->filename, nfields, keyfield, listable, inst->hashsize, inst->ignorenislike, inst->delimiter)) ){ 
		radlog(L_ERR, "rlm_passwd: can't build hashtable from passwd file");
		return -1;
	}
	len = strlen (inst->format)+ nfields * sizeof (char*) + sizeof (struct mypasswd) + 1;
	if (! (inst->pwdfmt = (struct mypasswd *)rad_malloc(len)) ){
		radlog(L_ERR, "rlm_passwd: memory allocation failed");
		release_hash_table(inst->ht);
		return -1;
	}
	if (!string_to_entry(inst->format, nfields, ':', inst->pwdfmt , len)) {
		radlog(L_ERR, "rlm_passwd: unable to convert format entry");
		release_hash_table(inst->ht);
		return -1;
	}
	if (*inst->pwdfmt->field[keyfield] == '*') inst->pwdfmt->field[keyfield]++;
	if (*inst->pwdfmt->field[keyfield] == ',') inst->pwdfmt->field[keyfield]++;
	if (!*inst->pwdfmt->field[keyfield]) {
		radlog(L_ERR, "rlm_passwd: key field is empty");
		release_hash_table(inst->ht);
		return -1;
	}
	if (! (da = dict_attrbyname (inst->pwdfmt->field[keyfield])) ) {
		radlog(L_ERR, "rlm_passwd: unable to resolve attribute: %s", inst->pwdfmt->field[keyfield]);
		release_hash_table(inst->ht);
		return -1;
	}
	inst->keyattr = da->attr;
	inst->keyattrtype = da->type;
	inst->nfields = nfields;
	inst->keyfield = keyfield;
	inst->listable = listable;
	radlog(L_INFO, "rlm_passwd: nfields: %d keyfield %d(%s) listable: %s", nfields, keyfield, inst->pwdfmt->field[keyfield], listable?"yes":"no");
	return 0;
	
#undef inst
}

static int passwd_detach (void *instance) {
#define inst ((struct passwd_instance *)instance)
	if(inst->ht) release_hash_table(inst->ht);
	free(instance);
	return 0;
#undef inst
}

static void addresult (struct passwd_instance * inst, VALUE_PAIR ** vp, struct mypasswd * pw)
{
	int i;
	VALUE_PAIR *newpair;
	
	for (i=0; i<inst->nfields; i++) {
		if (inst->pwdfmt->field[i] && *inst->pwdfmt->field[i] && pw->field[i] && i != inst->keyfield) {
			if (! (newpair = pairmake (inst->pwdfmt->field[i], pw->field[i], T_OP_EQ))) {
				radlog(L_AUTH, "rlm_passwd: Unable to create %s: %s", inst->pwdfmt->field[i], pw->field[i]);
				return;
			}
			radlog(L_INFO, "rlm_passwd: Added %s: %s", inst->pwdfmt->field[i], pw->field[i]);
			pairadd (vp, newpair);
		}
	}
}

static int passwd_authorize(void *instance, REQUEST *request)
{
#define inst ((struct passwd_instance *)instance)
	char * name;
	char buffer[1024];
	VALUE_PAIR * key;
	struct mypasswd * pw;
	int found = 0;
	
	if(!request || !request->packet ||!request->packet->vps)
	 return RLM_MODULE_INVALID;
	for (key = request->packet->vps;
	 key && (key = pairfind (key, inst->keyattr));
	  key = key->next ){
		switch (inst->keyattrtype) {
			case PW_TYPE_INTEGER:
				snprintf(buffer, 1024, "%u", key->lvalue);
				name = buffer;
				break;
			default:
				name = key->strvalue;
		}
		if (! (pw = get_pw_nam(name, inst->ht)) ) {
			continue;
		}
		do {
			addresult(inst, &request->config_items, pw);
		} while ( (pw = get_next(name, inst->ht)) );
		found++;
		if (!inst->allowmultiple) break;
	}
	if(!found) {
		return RLM_MODULE_NOTFOUND;
	}
	if (inst->authtype && (key = pairmake ("Auth-Type", inst->authtype, T_OP_EQ))) {
		radlog(L_INFO, "rlm_passwd: Adding Auth-Type: %s", inst->authtype);
		pairadd (&request->config_items, key);
	}
	return RLM_MODULE_OK;
	
#undef inst
}

module_t rlm_passwd = {
	"passwd",
	RLM_TYPE_THREAD_SAFE, 		/* type */
	NULL,				/* initialize */
	passwd_instantiate,		/* instantiation */
	{
		NULL,			/* authentication */
		passwd_authorize,	/* authorization */
		NULL,			/* pre-accounting */
		NULL,			/* accounting */
		NULL,			/* checksimul */
		NULL,			/* pre-proxy */
		NULL,			/* post-proxy */
		NULL			/* post-auth */
	},
	passwd_detach,			/* detach */
	NULL				/* destroy */
};
#endif /* TEST */
