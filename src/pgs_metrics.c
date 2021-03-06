#include "pgs_metrics.h"

const unsigned char g204_cmd[] = { 0x05, 0x01, 0x00, 0x03, 0x0d, 0x77, 0x77,
				   0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
				   0x65, 0x2e, 0x63, 0x6e, 0x00, 0x50 };

const char g204_http_req[] =
	"GET /generate_204 HTTP/1.1\r\nHost: www.google.cn\r\n\r\n";

static void do_ws_remote_request(pgs_bev_t *bev, void *ctx);
static void on_ws_g204_event(pgs_bev_t *bev, short events, void *ctx);
static void on_trojan_ws_g204_read(pgs_bev_t *bev, void *ctx);
static void on_v2ray_ws_g204_read(pgs_bev_t *bev, void *ctx);
static void on_trojan_gfw_g204_read(pgs_bev_t *bev, void *ctx);
static void on_trojan_gfw_g204_event(pgs_bev_t *bev, short events, void *ctx);
static void on_v2ray_tcp_g204_read(pgs_bev_t *bev, void *ctx);
static void on_v2ray_tcp_g204_event(pgs_bev_t *bev, short events, void *ctx);

static double elapse(struct timeval start_at)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	long seconds = now.tv_sec - start_at.tv_sec;
	long micros = ((seconds * 1000000) + now.tv_usec - start_at.tv_usec);
	return micros / 1000;
}

void get_metrics_g204_connect(pgs_ev_base_t *base, pgs_server_manager_t *sm,
			      int idx, pgs_logger_t *logger)
{
	const pgs_server_config_t *config = &sm->server_configs[idx];
	const pgs_buf_t *cmd = g204_cmd;
	pgs_size_t cmd_len = 20;
	pgs_metrics_task_ctx_t *mctx =
		pgs_metrics_task_ctx_new(base, sm, idx, logger, NULL);

	pgs_session_inbound_cbs_t inbound_cbs = { NULL, NULL, NULL, NULL,
						  NULL };
	pgs_session_outbound_cbs_t outbound_cbs = {
		on_ws_g204_event,	on_trojan_gfw_g204_event,
		on_ws_g204_event,	on_v2ray_tcp_g204_event,
		on_trojan_ws_g204_read, on_trojan_gfw_g204_read,
		on_v2ray_ws_g204_read,	on_v2ray_tcp_g204_read
	};

	pgs_session_outbound_t *ptr = pgs_session_outbound_new(
		config, idx, cmd, cmd_len, logger, base, mctx->dns_base, NULL,
		inbound_cbs, outbound_cbs, mctx,
		(free_ctx_fn *)pgs_metrics_task_ctx_free);
	mctx->outbound = ptr;
}

static void on_ws_g204_event(pgs_bev_t *bev, short events, void *ctx)
{
	pgs_metrics_task_ctx_t *mctx = ctx;
	if (events & BEV_EVENT_CONNECTED)
		do_ws_remote_request(bev, ctx);
	if (events & BEV_EVENT_ERROR)
		pgs_logger_error(mctx->logger, "Error from bufferevent");
	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		pgs_ssl_t *ssl = pgs_bev_openssl_get_ssl(bev);
		if (ssl)
			pgs_ssl_close(ssl);
		pgs_bev_free(bev);
		if (mctx)
			pgs_metrics_task_ctx_free(mctx);
	}
}
static void on_trojan_ws_g204_read(pgs_bev_t *bev, void *ctx)
{
	pgs_metrics_task_ctx_t *mctx = ctx;
	pgs_logger_debug(mctx->logger, "remote read triggered");
	pgs_evbuffer_t *output = pgs_bev_get_output(bev);
	pgs_evbuffer_t *input = pgs_bev_get_input(bev);

	pgs_size_t data_len = pgs_evbuffer_get_length(input);
	unsigned char *data = pgs_evbuffer_pullup(input, data_len);

	pgs_trojansession_ctx_t *trojan_s_ctx = mctx->outbound->ctx;
	if (!trojan_s_ctx->connected) {
		if (!strstr((const char *)data, "\r\n\r\n"))
			return;

		if (pgs_ws_upgrade_check((const char *)data)) {
			pgs_logger_error(mctx->logger,
					 "websocket upgrade fail!");
			on_ws_g204_event(bev, BEV_EVENT_ERROR, ctx);
		} else {
			//drain
			pgs_evbuffer_drain(input, data_len);
			trojan_s_ctx->connected = true;
			double connect_time = elapse(mctx->start_at);
			pgs_logger_debug(mctx->logger, "connect: %f",
					 connect_time);
			mctx->sm->server_stats[mctx->server_idx].connect_delay =
				connect_time;

			pgs_size_t len = strlen(g204_http_req);
			pgs_size_t head_len = trojan_s_ctx->head_len;

			if (head_len > 0)
				len += head_len;

			pgs_ws_write_head_text(output, len);

			if (head_len > 0) {
				pgs_evbuffer_add(output, trojan_s_ctx->head,
						 head_len);
				trojan_s_ctx->head_len = 0;
			}
			// x ^ 0 = x
			pgs_evbuffer_add(output, g204_http_req, len - head_len);
		}
	} else {
		double g204_time = elapse(mctx->start_at);
		pgs_logger_debug(mctx->logger, "g204: %f", g204_time);
		mctx->sm->server_stats[mctx->server_idx].g204_delay = g204_time;
	}
}

static void v2ray_ws_vmess_write_cb(pgs_evbuffer_t *writer, pgs_buf_t *data,
				    pgs_size_t len)
{
	pgs_ws_write_bin(writer, data, len);
}

static void on_v2ray_ws_g204_read(pgs_bev_t *bev, void *ctx)
{
	pgs_metrics_task_ctx_t *mctx = ctx;
	pgs_evbuffer_t *output = pgs_bev_get_output(bev);
	pgs_evbuffer_t *input = pgs_bev_get_input(bev);

	pgs_size_t data_len = pgs_evbuffer_get_length(input);
	unsigned char *data = pgs_evbuffer_pullup(input, data_len);

	pgs_vmess_ctx_t *v2ray_s_ctx = mctx->outbound->ctx;
	if (!v2ray_s_ctx->connected) {
		if (!strstr((const char *)data, "\r\n\r\n"))
			return;

		if (pgs_ws_upgrade_check((const char *)data)) {
			pgs_logger_error(mctx->logger,
					 "websocket upgrade fail!");
			on_ws_g204_event(bev, BEV_EVENT_ERROR, ctx);
		} else {
			//drain
			pgs_evbuffer_drain(input, data_len);
			v2ray_s_ctx->connected = true;
			double connect_time = elapse(mctx->start_at);
			pgs_logger_debug(mctx->logger, "connect: %f",
					 connect_time);
			mctx->sm->server_stats[mctx->server_idx].connect_delay =
				connect_time;
			pgs_size_t total_len = pgs_vmess_write(
				(const pgs_buf_t *)
					mctx->outbound->config->password,
				(const pgs_buf_t *)g204_http_req,
				strlen(g204_http_req), v2ray_s_ctx, output,
				(pgs_vmess_write_body_cb)&v2ray_ws_vmess_write_cb);
		}
	} else {
		double g204_time = elapse(mctx->start_at);
		pgs_logger_debug(mctx->logger, "g204: %f", g204_time);
		mctx->sm->server_stats[mctx->server_idx].g204_delay = g204_time;
	}
}
static void on_trojan_gfw_g204_read(pgs_bev_t *bev, void *ctx)
{
	// with data
	pgs_metrics_task_ctx_t *mctx = ctx;
	double g204_time = elapse(mctx->start_at);
	pgs_logger_debug(mctx->logger, "g204: %f", g204_time);
	mctx->sm->server_stats[mctx->server_idx].g204_delay = g204_time;
	on_trojan_gfw_g204_event(bev, BEV_EVENT_EOF, ctx);
}
static void on_trojan_gfw_g204_event(pgs_bev_t *bev, short events, void *ctx)
{
	// connect time and error handling
	pgs_metrics_task_ctx_t *mctx = ctx;
	if (events & BEV_EVENT_CONNECTED) {
		// set connected
		pgs_trojansession_ctx_t *sctx = mctx->outbound->ctx;
		sctx->connected = true;
		double connect_time = elapse(mctx->start_at);
		pgs_logger_debug(mctx->logger, "connect: %f", connect_time);
		mctx->sm->server_stats[mctx->server_idx].connect_delay =
			connect_time;
		// write request
		pgs_evbuffer_t *output = pgs_bev_get_output(bev);
		pgs_size_t len = strlen(g204_http_req);
		pgs_size_t head_len = sctx->head_len;
		if (head_len > 0)
			len += head_len;

		if (head_len > 0) {
			pgs_evbuffer_add(output, sctx->head, head_len);
			sctx->head_len = 0;
		}
		pgs_evbuffer_add(output, g204_http_req, len - head_len);
	}
	if (events & BEV_EVENT_ERROR)
		pgs_logger_error(mctx->logger, "Error from bufferevent");
	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		pgs_ssl_t *ssl = pgs_bev_openssl_get_ssl(bev);
		if (ssl)
			pgs_ssl_close(ssl);
		pgs_bev_free(bev);
		if (mctx)
			pgs_metrics_task_ctx_free(mctx);
	}
}
static void on_v2ray_tcp_g204_read(pgs_bev_t *bev, void *ctx)
{
	pgs_metrics_task_ctx_t *mctx = ctx;
	double g204_time = elapse(mctx->start_at);
	pgs_logger_debug(mctx->logger, "g204: %f", g204_time);
	mctx->sm->server_stats[mctx->server_idx].g204_delay = g204_time;
	// drop it, clean up
	on_v2ray_tcp_g204_event(bev, BEV_EVENT_EOF, ctx);
}
static void on_v2ray_tcp_g204_event(pgs_bev_t *bev, short events, void *ctx)
{
	pgs_metrics_task_ctx_t *mctx = ctx;
	if (events & BEV_EVENT_CONNECTED) {
		// set connected
		pgs_vmess_ctx_t *sctx = mctx->outbound->ctx;
		sctx->connected = true;
		double connect_time = elapse(mctx->start_at);
		pgs_logger_debug(mctx->logger, "connect: %f", connect_time);
		mctx->sm->server_stats[mctx->server_idx].connect_delay =
			connect_time;
		// write request
		pgs_evbuffer_t *output = pgs_bev_get_output(bev);
		pgs_size_t total_len = pgs_vmess_write(
			(const pgs_buf_t *)mctx->outbound->config->password,
			(const pgs_buf_t *)g204_http_req, strlen(g204_http_req),
			sctx, output,
			(pgs_vmess_write_body_cb)&pgs_evbuffer_add);
	}
	if (events & BEV_EVENT_ERROR)
		pgs_logger_error(mctx->logger, "Error from bufferevent");
	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		pgs_ssl_t *ssl = pgs_bev_openssl_get_ssl(bev);
		if (ssl)
			pgs_ssl_close(ssl);
		pgs_bev_free(bev);
		if (mctx)
			pgs_metrics_task_ctx_free(mctx);
	}
}

static void do_ws_remote_request(pgs_bev_t *bev, void *ctx)
{
	pgs_metrics_task_ctx_t *mctx = (pgs_metrics_task_ctx_t *)ctx;
	const pgs_server_config_t *config = mctx->outbound->config;
	// TODO: should assert here
	const pgs_server_ws_config_base_t *wsconfig = config->extra;

	pgs_logger_debug(mctx->logger, "do_ws_remote_request");

	pgs_ws_req(pgs_bev_get_output(mctx->outbound->bev),
		   wsconfig->websocket.hostname, config->server_address,
		   config->server_port, wsconfig->websocket.path);

	pgs_logger_debug(mctx->logger, "do_ws_remote_request done");
}

pgs_metrics_task_ctx_t *
pgs_metrics_task_ctx_new(pgs_ev_base_t *base, pgs_server_manager_t *sm, int idx,
			 pgs_logger_t *logger, pgs_session_outbound_t *outbound)
{
	pgs_metrics_task_ctx_t *ptr =
		pgs_malloc(sizeof(pgs_metrics_task_ctx_t));
	ptr->base = base;
	ptr->dns_base =
		pgs_ev_dns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS);
	ptr->sm = sm;
	ptr->server_idx = idx;
	ptr->logger = logger;
	ptr->outbound = outbound;
	gettimeofday(&ptr->start_at, NULL);
	return ptr;
}

void pgs_metrics_task_ctx_free(pgs_metrics_task_ctx_t *ptr)
{
	if (ptr) {
		if (ptr->outbound) {
			pgs_session_outbound_free(ptr->outbound);
			ptr->outbound = NULL;
		}
		if (ptr->dns_base) {
			pgs_ev_dns_base_free(ptr->dns_base, 0);
			ptr->dns_base = NULL;
		}
		pgs_free(ptr);
		ptr = NULL;
	}
}
