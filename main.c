#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "glib-2.0/gmodule.h"
#include "libxml/parser.h"
#include "libxml/xmlmemory.h"

#define FONT_COUNT 3

void die(const char *msg);
GHashTable *get_font_usages(const char *screens_path);

void print_usage(gpointer key, gpointer value, gpointer user_data)
{
	(void)user_data;

	printf("%s:", (const char *)key);
	for (unsigned i = 0; i < FONT_COUNT; ++i) {
		if (((bool *)value)[i])
			printf(" %d", i);
	}
	printf("\n");
}

int main(int argc, char *argv[])
{
	GHashTable *font_usages;

	if (argc < 2)
		die("usage: spec-glyphs SCREENS_XML");

	font_usages = get_font_usages(argv[1]);
	if (!font_usages)
		die("failed to process screens XML");
	g_hash_table_foreach(font_usages, print_usage, NULL);

	g_hash_table_remove_all(font_usages);
}

/// Prints `msg` to `stderr` then exits with status 1
void die(const char *msg)
{
	fprintf(stderr, "spec-glyphs: %s\n", msg);
	exit(1);
}

/// Builds a hash table mapping text IDs to an array of bools such that
/// for some text ID `t` and font index `f`, `t` is drawn in `f` if and
/// only if `g_hash_table_lookup(ht, t)[f]` is true (where ht is the
/// produced hash table). Returns `NULL` on error.
GHashTable *get_font_usages(const char *screens_path)
{
	bool read_screen(GHashTable *usages, xmlNode *screen);

	xmlDoc *doc;
	xmlNode *root, *screen;
	GHashTable *usages;

	doc = xmlParseFile(screens_path);
	if (!doc)
		return NULL;
	root = xmlDocGetRootElement(doc);
	if (!root)
		return NULL;

	usages = g_hash_table_new_full(g_str_hash, g_str_equal, xmlFree, free);
	if (usages == NULL)
		return NULL;

	for (screen = root->xmlChildrenNode; screen; screen = screen->next) {
		if (xmlStrcmp(screen->name, (const xmlChar *)"screen") != 0)
			continue;

		if (!read_screen(usages, screen))
			return NULL;
	}

	xmlFreeDoc(doc);
	return usages;
} 

bool read_screen(GHashTable *usages, xmlNode *screen)
{
	xmlNode *text;
	xmlChar *text_id, *font_str;
	unsigned font;
	bool *usage;

	xmlChar *screenName = xmlGetProp(screen, (const xmlChar *)"name");
	xmlFree(screenName);

	for (text = screen->xmlChildrenNode; text; text = text->next) {
		if (xmlStrcmp(text->name, (const xmlChar *)"text") != 0)
			continue;

		// libxml gives children content nodes the name "text", so we
		// expect to see text nodes without a value attribute
		text_id = xmlGetProp(text, (const xmlChar *)"value");
		if (!text_id)
			continue;

		font_str = xmlGetProp(text, (const xmlChar *)"font");
		if (!font_str)
			return false;
		font = atoi((const char *)font_str);
		xmlFree(font_str);
		if (font >= FONT_COUNT)
			return false;

		usage = g_hash_table_lookup(usages, text_id);
		if (!usage) {
			usage = calloc(FONT_COUNT, sizeof(bool));
			g_hash_table_insert(usages, text_id, usage);
		}
		usage[font] = true;
	}

	return true;
}
