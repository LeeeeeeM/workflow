/*
  Copyright (c) 2019 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Authors: Wu Jiaxu (wujiaxu@sogou-inc.com)
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <chrono>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <openssl/ssl.h>
#include "list.h"
#include "rbtree.h"
#include "WFGlobal.h"
#include "CommScheduler.h"
#include "EndpointParams.h"
#include "RouteManager.h"
#include "StringUtil.h"

#define GET_CURRENT_SECOND	std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
#define MTTR_SECOND			30

using RouteTargetTCP = RouteManager::RouteTarget;

class RouteTargetUDP : public RouteManager::RouteTarget
{
private:
	virtual int create_connect_fd()
	{
		const struct sockaddr *addr;
		socklen_t addrlen;

		this->get_addr(&addr, &addrlen);
		return socket(addr->sa_family, SOCK_DGRAM, 0);
	}
};

class RouteTargetSCTP : public RouteManager::RouteTarget
{
private:
#ifdef IPPROTO_SCTP
	virtual int create_connect_fd()
	{
		const struct sockaddr *addr;
		socklen_t addrlen;

		this->get_addr(&addr, &addrlen);
		return socket(addr->sa_family, SOCK_STREAM, IPPROTO_SCTP);
	}
#else
	virtual int create_connect_fd()
	{
		errno = EPROTONOSUPPORT;
		return -1;
	}
#endif
};

using RouteTargetTCP_SSL = RouteTargetTCP;

/* To support TLS SNI. */
class RouteTargetTCP_TLS_SNI : public RouteTargetTCP_SSL
{
private:
	virtual int init_ssl(SSL *ssl)
	{
		if (SSL_set_tlsext_host_name(ssl, this->hostname.c_str()) > 0)
			return 0;
		else
			return -1;
	}

private:
	std::string hostname;

public:
	RouteTargetTCP_TLS_SNI(const std::string& name) : hostname(name)
	{
	}
};

using RouteTargetSCTP_SSL = RouteTargetSCTP;

class RouteTargetSCTP_TLS_SNI : public RouteTargetSCTP_SSL
{
private:
	virtual int init_ssl(SSL *ssl)
	{
		if (SSL_set_tlsext_host_name(ssl, this->hostname.c_str()) > 0)
			return 0;
		else
			return -1;
	}

private:
	std::string hostname;

public:
	RouteTargetSCTP_TLS_SNI(const std::string& name) : hostname(name)
	{
	}
};

//  protocol_name\n user\n pass\n dbname\n ai_addr ai_addrlen \n....
//

struct RouteParams
{
	enum TransportType transport_type;
	const struct addrinfo *addrinfo;
	uint64_t key;
	SSL_CTX *ssl_ctx;
	size_t max_connections;
	int connect_timeout;
	int response_timeout;
	int ssl_connect_timeout;
	bool use_tls_sni;
	const std::string& hostname;
};

class RouteResultEntry
{
public:
	struct rb_node rb;
	CommSchedObject *request_object;
	CommSchedGroup *group;
	std::mutex mutex;
	std::vector<RouteManager::RouteTarget *> targets;
	struct list_head breaker_list;
	uint64_t key;
	int nleft;
	int nbreak;

	RouteResultEntry():
		request_object(NULL),
		group(NULL)
	{
		INIT_LIST_HEAD(&this->breaker_list);
		this->nleft = 0;
		this->nbreak = 0;
	}

public:
	int init(const struct RouteParams *params);
	void deinit();

	void notify_unavailable(RouteManager::RouteTarget *target);
	void notify_available(RouteManager::RouteTarget *target);
	void check_breaker();

private:
	void free_list();
	RouteManager::RouteTarget *create_target(const struct RouteParams *params,
											 const struct addrinfo *addrinfo);
	int add_group_targets(const struct RouteParams *params);
};

struct __breaker_node
{
	RouteManager::RouteTarget *target;
	int64_t timeout;
	struct list_head breaker_list;
};

RouteManager::RouteTarget *
RouteResultEntry::create_target(const struct RouteParams *params,
								const struct addrinfo *addr)
{
	RouteManager::RouteTarget *target;

	switch (params->transport_type)
	{
	case TT_TCP:
		target = new RouteTargetTCP();
		break;
	case TT_UDP:
		target = new RouteTargetUDP();
		break;
	case TT_SCTP:
		target = new RouteTargetSCTP();
		break;
	case TT_TCP_SSL:
		if (params->use_tls_sni)
			target = new RouteTargetTCP_TLS_SNI(params->hostname);
		else
			target = new RouteTargetTCP_SSL;
		break;
	case TT_SCTP_SSL:
		if (params->use_tls_sni)
			target = new RouteTargetSCTP_TLS_SNI(params->hostname);
		else
			target = new RouteTargetSCTP_SSL;
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	if (target->init(addr->ai_addr, addr->ai_addrlen, params->ssl_ctx,
					 params->connect_timeout, params->ssl_connect_timeout,
					 params->response_timeout, params->max_connections) < 0)
	{
		delete target;
		target = NULL;
	}

	return target;
}

int RouteResultEntry::init(const struct RouteParams *params)
{
	const struct addrinfo *addr = params->addrinfo;
	RouteManager::RouteTarget *target;

	if (addr == NULL)//0
	{
		errno = EINVAL;
		return -1;
	}

	if (addr->ai_next == NULL)//1
	{
		target = this->create_target(params, addr);
		if (target)
		{
			this->targets.push_back(target);
			this->request_object = target;
			this->key = params->key;
			return 0;
		}

		return -1;
	}

	this->group = new CommSchedGroup();
	if (this->group->init() >= 0)
	{
		if (this->add_group_targets(params) >= 0)
		{
			this->request_object = this->group;
			this->key = params->key;
			return 0;
		}

		this->group->deinit();
	}

	delete this->group;
	return -1;
}

int RouteResultEntry::add_group_targets(const struct RouteParams *params)
{
	RouteManager::RouteTarget *target;
	const struct addrinfo *addr;

	for (addr = params->addrinfo; addr; addr = addr->ai_next)
	{
		target = this->create_target(params, addr);
		if (target)
		{
			if (this->group->add(target) >= 0)
			{
				this->targets.push_back(target);
				this->nleft++;
				continue;
			}

			target->deinit();
			delete target;
		}

		for (auto *target : this->targets)
		{
			this->group->remove(target);
			target->deinit();
			delete target;
		}

		return -1;
	}

	return 0;
}

void RouteResultEntry::deinit()
{
	for (auto *target : this->targets)
	{
		if (this->group)
			this->group->remove(target);

		target->deinit();
		delete target;
	}

	if (this->group)
	{
		this->group->deinit();
		delete this->group;
	}

	struct list_head *pos, *tmp;
	__breaker_node *node;

	list_for_each_safe(pos, tmp, &this->breaker_list)
	{
		node = list_entry(pos, __breaker_node, breaker_list);
		list_del(pos);
		delete node;
	}
}

void RouteResultEntry::notify_unavailable(RouteManager::RouteTarget *target)
{
	if (this->targets.size() <= 1)
		return;

	int errno_bak = errno;
	std::lock_guard<std::mutex> lock(this->mutex);

	if (this->nleft <= 1)
		return;

	if (this->group->remove(target) < 0)
	{
		errno = errno_bak;
		return;
	}

	auto *node = new __breaker_node;

	node->target = target;
	node->timeout = GET_CURRENT_SECOND + MTTR_SECOND;
	list_add_tail(&node->breaker_list, &this->breaker_list);
	this->nbreak++;
	this->nleft--;
}

void RouteResultEntry::notify_available(RouteManager::RouteTarget *target)
{
	if (this->targets.size() <= 1 || this->nbreak == 0)
		return;

	int errno_bak = errno;
	std::lock_guard<std::mutex> lock(this->mutex);

	if (this->group->add(target) == 0)
		this->nleft++;
	else
		errno = errno_bak;
}

void RouteResultEntry::check_breaker()
{
	if (this->targets.size() <= 1 || this->nbreak == 0)
		return;

	struct list_head *pos, *tmp;
	__breaker_node *node;
	int errno_bak = errno;
	int64_t cur_time = GET_CURRENT_SECOND;
	std::lock_guard<std::mutex> lock(this->mutex);

	list_for_each_safe(pos, tmp, &this->breaker_list)
	{
		node = list_entry(pos, __breaker_node, breaker_list);
		if (cur_time >= node->timeout)
		{
			if (this->group->add(node->target) == 0)
				this->nleft++;
			else
				errno = errno_bak;

			list_del(pos);
			delete node;
			this->nbreak--;
		}
	}
}

static inline int __addr_cmp(const struct addrinfo *x, const struct addrinfo *y)
{
	//todo ai_protocol
	if (x->ai_addrlen == y->ai_addrlen)
		return memcmp(x->ai_addr, y->ai_addr, x->ai_addrlen);
	else if (x->ai_addrlen < y->ai_addrlen)
		return -1;
	else
		return 1;
}

static inline bool __addr_less(const struct addrinfo *x, const struct addrinfo *y)
{
	return __addr_cmp(x, y) < 0;
}

static uint64_t __fnv_hash(const unsigned char *data, size_t size)
{
	uint64_t hash = 14695981039346656037ULL;

	while (size)
	{
		hash ^= (const uint64_t)*data++;
		hash *= 1099511628211ULL;
		size--;
	}

	return hash;
}

static uint64_t __generate_key(enum TransportType type,
							   const struct addrinfo *addrinfo,
							   const std::string& other_info,
							   const struct EndpointParams *ep_params,
							   const std::string& hostname,
							   SSL_CTX *ssl_ctx)
{
	const int params[] = {
		ep_params->address_family, (int)ep_params->max_connections,
		ep_params->connect_timeout, ep_params->response_timeout
	};
	std::string buf((const char *)&type, sizeof (enum TransportType));

	if (!other_info.empty())
		buf += other_info;

	buf.append((const char *)params, sizeof params);
	if (type == TT_TCP_SSL || type == TT_SCTP_SSL)
	{
		buf.append((const char *)ssl_ctx, sizeof (void *));
		buf.append((const char *)&ep_params->ssl_connect_timeout, sizeof (int));
		if (ep_params->use_tls_sni)
		{
			buf += hostname;
			buf += '\n';
		}
	}

	if (addrinfo->ai_next)
	{
		std::vector<const struct addrinfo *> sorted_addr;

		sorted_addr.push_back(addrinfo);
		addrinfo = addrinfo->ai_next;
		do
		{
			sorted_addr.push_back(addrinfo);
			addrinfo = addrinfo->ai_next;
		} while (addrinfo);

		std::sort(sorted_addr.begin(), sorted_addr.end(), __addr_less);
		for (const struct addrinfo *p : sorted_addr)
		{
			buf.append((const char *)&p->ai_addrlen, sizeof (socklen_t));
			buf.append((const char *)p->ai_addr, p->ai_addrlen);
		}
	}
	else
	{
		buf.append((const char *)&addrinfo->ai_addrlen, sizeof (socklen_t));
		buf.append((const char *)addrinfo->ai_addr, addrinfo->ai_addrlen);
	}

	return __fnv_hash((const unsigned char *)buf.c_str(), buf.size());
}

RouteManager::~RouteManager()
{
	RouteResultEntry *entry;

	while (cache_.rb_node)
	{
		entry = rb_entry(cache_.rb_node, RouteResultEntry, rb);
		rb_erase(cache_.rb_node, &cache_);
		entry->deinit();
		delete entry;
	}
}

int RouteManager::get(enum TransportType type,
					  const struct addrinfo *addrinfo,
					  const std::string& other_info,
					  const struct EndpointParams *ep_params,
					  const std::string& hostname, SSL_CTX *ssl_ctx,
					  RouteResult& result)
{
	if (type == TT_TCP_SSL || type == TT_SCTP_SSL)
	{
		static SSL_CTX *global_client_ctx = WFGlobal::get_ssl_client_ctx();
		if (ssl_ctx == NULL)
			ssl_ctx = global_client_ctx;
	}
	else
		ssl_ctx = NULL;

	uint64_t key = __generate_key(type, addrinfo, other_info, ep_params,
								  hostname, ssl_ctx);
	struct rb_node **p = &cache_.rb_node;
	struct rb_node *parent = NULL;
	RouteResultEntry *bound = NULL;
	RouteResultEntry *entry;
	std::lock_guard<std::mutex> lock(mutex_);

	while (*p)
	{
		parent = *p;
		entry = rb_entry(*p, RouteResultEntry, rb);
		if (key <= entry->key)
		{
			bound = entry;
			p = &(*p)->rb_left;
		}
		else
			p = &(*p)->rb_right;
	}

	if (bound && bound->key == key)
	{
		entry = bound;
		entry->check_breaker();
	}
	else
	{
		struct RouteParams params = {
			.transport_type			=	type,
			.addrinfo 				=	addrinfo,
			.key					=	key,
			.ssl_ctx				=	ssl_ctx,
			.max_connections		=	ep_params->max_connections,
			.connect_timeout		=	ep_params->connect_timeout,
			.response_timeout		=	ep_params->response_timeout,
			.ssl_connect_timeout	=	ep_params->ssl_connect_timeout,
			.use_tls_sni			=	ep_params->use_tls_sni,
			.hostname				=	hostname,
		};

		entry = new RouteResultEntry;
		if (entry->init(&params) >= 0)
		{
			rb_link_node(&entry->rb, parent, p);
			rb_insert_color(&entry->rb, &cache_);
		}
		else
		{
			delete entry;
			return -1;
		}
	}

	result.cookie = entry;
	result.request_object = entry->request_object;
	return 0;
}

void RouteManager::notify_unavailable(void *cookie, CommTarget *target)
{
	if (cookie && target)
		((RouteResultEntry *)cookie)->notify_unavailable((RouteTarget *)target);
}

void RouteManager::notify_available(void *cookie, CommTarget *target)
{
	if (cookie && target)
		((RouteResultEntry *)cookie)->notify_available((RouteTarget *)target);
}

