/**
 * @file google_source.c Google reader feed list source support
 * 
 * Copyright (C) 2007 Lars Lindner <lars.lindner@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <glib.h>
#include <libxml/xpath.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <unistd.h>
#include <string.h>

#include "common.h"
#include "conf.h"
#include "debug.h"
#include "node.h"
#include "fl_sources/google_source-cb.h"
#include "fl_sources/node_source.h"
#include "fl_sources/opml_source.h"

/** default Google reader subscription list update interval = once a day */
#define GOOGLE_SOURCE_UPDATE_INTERVAL 60*60*24

typedef struct reader {
	nodePtr		root;	/**< the root node in the feed list */
	gchar		*sid;	/**< session id */
	GTimeVal	*lastSubscriptionListUpdate;
} *readerPtr;


static void
google_source_merge_feed(xmlNodePtr match, gpointer user_data)
{
	readerPtr	reader = (readerPtr)user_data;
	nodePtr		node;
	GSList		*iter;
	xmlNodePtr	xml;
	xmlChar		*title, *id;
	
	xml = common_xpath_find (match, "./string[@name='title']");
	if (xml)
		title = xmlNodeListGetString (xml->doc, xml->xmlChildrenNode, 1);
		
	xml = common_xpath_find (match, "./string[@name='id']");
	if (xml)
		id = xmlNodeListGetString (xml->doc, xml->xmlChildrenNode, 1);
		
	/* check if node already exists */
	iter = reader->root->children;
	while (iter) {
		node = (nodePtr)iter->data;
		if (g_str_equal (node_get_title (node), title))
			return;
		iter = g_slist_next (iter);
	}
	
	/* Note: ids look like "feed/http://rss.slashdot.org" */
	if (id && title) {
		debug2 (DEBUG_UPDATE, "adding %s (%s)", title, id + 5);
		node = node_new ();
		node_set_title (node, title);
		node_set_type (node, feed_get_node_type ());
		node_set_data (node, feed_new ());
		node_set_subscription (node, subscription_new (id + 5, NULL, NULL));
		node_add_child (reader->root, node, -1);
		node_request_update (node, FEED_REQ_RESET_TITLE);
	}

	if (id)
		xmlFree (id);
	if (title)
		xmlFree (title);
}

static void
google_source_subscriptions_cb (requestPtr request)
{
	nodePtr		node = (nodePtr)request->user_data;
	readerPtr	reader = (readerPtr)node->data;
	xmlNodePtr	root;
	xmlDocPtr	doc;
	
	debug1(DEBUG_UPDATE, "Google Reader subscription list download finished data=%d", request->data);

	node->available = FALSE;

	if (request->data) {
		doc = common_parse_xml(request->data, request->size, FALSE, NULL);
		if(doc) {		
			root = xmlDocGetRootElement(doc);
			
			/* Go through all existing nodes and remove those whose
			   URLs are not in new feed list. Also removes those URLs
			   from the list that have corresponding existing nodes. */
			// FIXME: implement me!

			opml_source_export (node);	/* save new feed list tree to disk 
			                                   to ensure correct document in 
							   next step */

			common_xpath_foreach_match(root, "/object/list[@name='subscriptions']/object",
						   google_source_merge_feed,
						   (gpointer)reader);
						   
			opml_source_export (node);	/* save new feeds to feed list */
						   
			node->available = TRUE;
		}
	}
	g_free (request->data);
}

static void
google_source_login_cb (requestPtr request)
{
	nodePtr		node = (nodePtr)request->user_data;
	readerPtr	reader = (readerPtr)node->data;
	gchar		*tmp;
	
	debug0 (DEBUG_UPDATE, "google login processing...");
	
	if (node->subscription->updateError) {
		g_free (node->subscription->updateError);
		node->subscription->updateError = NULL;
	}
	
	g_assert (!reader->sid);
	
	if (request->data)
		tmp = strstr (request->data, "SID=");
		
	if (tmp) {
		reader->sid = tmp;
		tmp = strchr(tmp, '\n');
		if(tmp)
			*tmp = '\0';
		reader->sid = g_strdup_printf ("Cookie: %s\r\n", reader->sid);
		debug1 (DEBUG_UPDATE, "google reader SID found: %s", reader->sid);
		node->available = TRUE;
		
		/* now that we are authenticated retrigger updating to start data retrieval */
		node_request_update (node, 0);
	} else {
		debug0 (DEBUG_UPDATE, "google reader login failed! no SID found in result!");
		node->available = FALSE;
		node->subscription->updateError = g_strdup (_("Google Reader login failed!"));
	}
	g_free (request->data);
}

/* authenticate to receive SID... */
static void
google_source_login (nodePtr node, GTimeVal *now)
{
	requestPtr	request;
	readerPtr	reader = (readerPtr)node->data;
	
	if (!reader) {
		node->data = reader = g_new0 (struct reader, 1);
		reader->root = node;
	}
	
	request = update_request_new (node);
	subscription_prepare_request (node->subscription, request, 0, now);
	request->options = node->subscription->updateOptions;
	request->source = g_strdup_printf ("https://www.google.com/accounts/ClientLogin?service=reader&Email=%s&Passwd=%s&source=liferea&continue=http://www.google.com",
	                                    node->subscription->updateOptions->username,
	                                    node->subscription->updateOptions->password);
	request->callback = google_source_login_cb;
	request->priority = 1;
	request->user_data = node;
	debug2 (DEBUG_UPDATE, "login to Google Reader source %s (node id %s)", request->source, node->id);
	update_execute_request (request);
}

static void
google_source_update_subscription_list (nodePtr node, GTimeVal *now)
{
	requestPtr	request;
	readerPtr	reader = (readerPtr)node->data;
	
	if (!reader) {
		/* not logged in yet, successful login will
		   trigger an update automatically, so return
		   after calling async login method ... */
		google_source_login (node, now);
		return;
	}
	
	request = update_request_new (node);
	subscription_prepare_request (node->subscription, request, 0, now);
	request->options = node->subscription->updateOptions;
	node->subscription->updateState->cookies = reader->sid;
	request->source = g_strdup ("http://www.google.com/reader/api/0/subscription/list");
	request->callback = google_source_subscriptions_cb;
	request->priority = 1;
	request->user_data = node;
	debug2 (DEBUG_UPDATE, "updating Google Reader source %s (node id %s)", request->source, node->id);
	update_execute_request (request);
}

/** called during import and when subscribing, we will do
    node_add_child() only when subscribing */
void google_source_setup(nodePtr parent, nodePtr node) {

	node->icon = create_pixbuf("fl_google.png");
	
	node_set_type(node, node_source_get_node_type());
	if(parent) {
		gint pos;
		ui_feedlist_get_target_folder(&pos);
		node_add_child(parent, node, pos);
	}
}

static void
google_source_auto_update (nodePtr node, GTimeVal *now)
{
	/* do daily updates for the feed list and feed updates according to the default interval */
	if (node->subscription->updateState->lastPoll.tv_sec + GOOGLE_SOURCE_UPDATE_INTERVAL <= now->tv_sec)
		google_source_update_subscription_list (node, now);
}

static void google_source_init (void) { }

static void google_source_deinit (void) { }

/* node source type definition */

static struct nodeSourceType nst = {
	NODE_SOURCE_TYPE_API_VERSION,
	"fl_google",
	N_("Google Reader"),
	N_("Integrate the feed list of your Google Reader account. Liferea will "
	   "present your Google Reader subscription as a read-only subtree in the feed list."),
	NODE_SOURCE_CAPABILITY_DYNAMIC_CREATION,
	google_source_init,
	google_source_deinit,
	ui_google_source_get_account_info,
	opml_source_remove,
	opml_source_import,
	opml_source_export,
	opml_source_get_feedlist,
	google_source_update_subscription_list,
	google_source_auto_update
};

nodeSourceTypePtr
google_source_get_type(void)
{
	return &nst;
}