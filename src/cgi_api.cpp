///////////////////////////////////////////////////////////////////////////////
//                                                                             
//  Copyright (C) 2008-2012  Artyom Beilis (Tonkikh) <artyomtnk@yahoo.com>     
//                                                                             
//  See accompanying file COPYING.TXT file for licensing details.
//
///////////////////////////////////////////////////////////////////////////////
#define CPPCMS_SOURCE
#include <cppcms/url_dispatcher.h>
#include <cppcms/http_context.h>
#include <cppcms/http_request.h>
#include <cppcms/http_response.h>
#include <cppcms/forwarder.h>
#include <cppcms/http_content_filter.h>
#include "http_protocol.h"
#include <cppcms/service.h>
#include "service_impl.h"
#include "cached_settings.h"
#include <cppcms/json.h>
#include "cgi_api.h"
#include <cppcms/util.h>
#include <scgi_header.h>
#include <stdlib.h>
#include <cppcms/config.h>
#include "binder.h"
#include "response_headers.h"

#include <booster/log.h>
#include <booster/aio/endpoint.h>
#include <booster/aio/aio_category.h>
#include <booster/aio/socket.h>
#include <booster/aio/buffer.h>


namespace cppcms { namespace impl { namespace cgi {

	//
	// Special forwarder from generic CGI to SCGI
	//
	struct connection::cgi_forwarder : public booster::enable_shared_from_this<connection::cgi_forwarder> {
	public:
		cgi_forwarder(booster::shared_ptr<connection> c,std::string ip,int port) :
			conn_(c),
			scgi_(c->get_io_service()),
			ep_(ip,port)
		{
			booster::aio::endpoint ep(ip,port);
			booster::system::error_code e;
			scgi_.open(ep.family(),e);
			if(e) {	return;	}
		}
		void async_run()
		{
			scgi_.async_connect(ep_,mfunc_to_event_handler(&cgi_forwarder::on_connected,shared_from_this()));
		}
	private:
		void on_connected(booster::system::error_code const &e)
		{
			if(e) return;
			header_ = make_scgi_header(conn_->getenv(),0);
			scgi_.async_write(
				booster::aio::buffer(header_),
				mfunc_to_io_handler(&cgi_forwarder::on_header_sent,shared_from_this()));
		}
		void on_header_sent(booster::system::error_code const &e,size_t n)
		{
			if(e || n!=header_.size())
				return;
			header_.clear();
			content_length_ = conn_->env_content_length();
			if(content_length_ > 0) {
				post_.resize( content_length_ > 8192 ? 8192 : content_length_,0);
				write_post();
			}
			else {
				response_.resize(8192);
				read_response();
			}
		}
		void write_post()
		{
			if(content_length_ > 0) {
				if(content_length_ <  (long long)(post_.size())) {
					post_.resize(content_length_);
				}
				conn_->async_read_some(&post_.front(),post_.size(),
					mfunc_to_io_handler(&cgi_forwarder::on_post_data_read,shared_from_this()));
			}
			else {
				response_.swap(post_);
				response_.resize(8192);
				read_response();
			}
		}
		void on_post_data_read(booster::system::error_code const &e,size_t len)
		{
			if(e)  { cleanup(); return; }
			conn_->on_async_read_complete();
			scgi_.async_write(
				booster::aio::buffer(&post_.front(),len),
				mfunc_to_io_handler(&cgi_forwarder::on_post_data_written,shared_from_this()));
		}
		void on_post_data_written(booster::system::error_code const &e,size_t len)
		{
			if(e) { return; }
			content_length_ -= len;
			write_post();
		}
		
		void read_response() 
		{
			conn_->async_read_eof(mfunc_to_handler(&cgi_forwarder::cleanup,shared_from_this()));
			scgi_.async_read_some(booster::aio::buffer(response_),
						mfunc_to_io_handler(&cgi_forwarder::on_response_read,shared_from_this()));
		}
		void on_response_read(booster::system::error_code const &e,size_t len)
		{
			if(e) {
				conn_->async_write(booster::aio::const_buffer(),true,mfunc_to_event_handler(&cgi_forwarder::cleanup,shared_from_this()));
				return;
			}
			else {
				char const *data = &response_.front();
				headers_parser_.consume(data,len,*conn_);
				if(len == 0) {
					conn_->get_io_service().post(std::bind(&cgi_forwarder::on_response_written,
										shared_from_this(),
										booster::system::error_code()));
					return;
				}
				conn_->async_write(booster::aio::buffer(data,len),false,mfunc_to_event_handler(&cgi_forwarder::on_response_written,shared_from_this()));
			}
		}
		void on_response_written(booster::system::error_code const &e)
		{
			if(e) { cleanup(); return; }
			scgi_.async_read_some(booster::aio::buffer(response_),
				mfunc_to_io_handler(&cgi_forwarder::on_response_read,shared_from_this()));
		}

		void cleanup()
		{
			conn_->do_eof();
			booster::system::error_code e;
			scgi_.shutdown(booster::aio::stream_socket::shut_rdwr,e);
			scgi_.close(e);
		}
		void cleanup(booster::system::error_code const &)
		{
			cleanup();
		}

		booster::shared_ptr<connection> conn_;
		booster::aio::stream_socket scgi_;
		booster::aio::endpoint ep_;
		long long int content_length_;
		std::string header_;
		std::vector<char> post_;
		std::vector<char> response_;
		cppcms::impl::cgi_headers_parser headers_parser_;

	};








cppcms::service &connection::service()
{
	return *service_;
}
booster::shared_ptr<connection> connection::self()
{
	return shared_from_this();
}

void connection::async_prepare_request(	http::context *context,
					ehandler const &h)
{
	booster::system::error_code e;
	socket().set_non_blocking(true,e);
	if(e) {
		BOOSTER_WARNING("cppcms") << "Failed to set nonblocking mode in socket " << e.message();
		get_io_service().post(func_to_handler(h,http::context::operation_aborted));
		return;
	}
	async_read_headers(mfunc_to_event_handler(&connection::on_headers_read,self(),context,h));
}

void connection::on_headers_read(booster::system::error_code const &e,http::context *context,ehandler const &h)
{
	if(e)  {
		set_error(h,e.message());
		return;
	}
	forwarder::address_type addr = service().forwarder().check_forwading_rules(
		env_http_host(),
		env_script_name(),
		env_path_info());
	
	if(addr.second != 0 && !addr.first.empty()) {
		booster::shared_ptr<cgi_forwarder> f(new cgi_forwarder(self(),addr.first,addr.second));
		f->async_run();
		h(http::context::operation_aborted);
		return;
	}
	load_content(context,h);
}

void connection::aync_wait_for_close_by_peer(booster::callback<void()> const &on_eof)
{
	async_read_eof(mfunc_to_handler(&connection::handle_eof,self(),on_eof));
}

void connection::handle_eof(callback const &on_eof)
{
	on_eof();
}

void connection::set_error(ehandler const &h,std::string s)
{
	error_=s;
	h(http::context::operation_aborted);
}

void connection::handle_http_error(int code,http::context *context,ehandler const &h)
{
	async_chunk_.clear();
	if(!context->response().some_output_was_written()) {
		std::ostringstream ss;
		context->response().status(code);
		context->response().write_http_headers();
		cppcms::http::response::make_error_response_html_body(code,ss);
		async_chunk_ += ss.str();
	}
	else {
		booster::system::error_code e;
		context->response().flush_async_chunk(e);
	}
    error_state_ = true;
	async_write(booster::aio::buffer(async_chunk_),true,
		mfunc_to_event_handler(
			&connection::handle_http_error_eof,
			self(),
			code,
			h));
}

void connection::handle_http_error_eof(
	booster::system::error_code const &e,
	int code,
	ehandler const &h)
{
	if(e)  {
		set_error(h,e.message());
		return;
	}
	do_eof();
	set_error(h,http::response::status_to_string(code));
}



void connection::load_content(http::context *context,ehandler const &h)
{
	int status=0;
	if((status = context->on_headers_ready())!=0) {
		handle_http_error(status,context,h);
		return;
	}

	if(context->request().content_length() > 0) {
		std::pair<char *,size_t> buffer = context->request().get_buffer();
		async_read_some(buffer.first,buffer.second,
			mfunc_to_io_handler(&connection::on_some_content_read,
				self(),
				context,
				h));
	}
	else  {
		on_async_read_complete();
		h(http::context::operation_completed);
	}
}


void connection::on_some_content_read(booster::system::error_code const &e,size_t n,http::context *context,ehandler const &h)
{
	if(e) { set_error(h,e.message()); return; }


	int status = context->on_content_progress(n);
	if(status !=0) {
		 handle_http_error(status,context,h);
		 return;
	}

	std::pair<char *,size_t> buffer = context->request().get_buffer();

	if(buffer.second==0) {
		on_async_read_complete();
		h(http::context::operation_completed);
		return;
	}
	else {
		async_read_some(buffer.first,buffer.second,
			mfunc_to_io_handler(&connection::on_some_content_read,
				self(),
				context,
				h));
	}
}

bool connection::is_reuseable()
{
	return error_.empty() && keep_alive();
}

std::string connection::last_error()
{
	return error_;
}

struct connection::async_write_binder : public booster::callable<void(booster::system::error_code const &)> {
	typedef booster::shared_ptr<cppcms::impl::cgi::connection> conn_type;
	
	conn_type conn;
	ehandler h;
	bool complete_response;

	void reset()
	{
		h=ehandler();
		conn.reset();
		complete_response = false;
	}
	void operator()(booster::system::error_code const &e)
	{
		if(complete_response) {
			conn->do_eof();
		}
		h(e ? cppcms::http::context::operation_aborted : cppcms::http::context::operation_completed );
		if(!conn->cached_async_write_binder_) {
			conn->cached_async_write_binder_ = this;
			reset();
		}
	}
};

void connection::async_write_response(	http::response &response,
					bool complete_response,
					ehandler const &h)
{
	//  prepare cached binder
	booster::intrusive_ptr<connection::async_write_binder> tmp;
	if(cached_async_write_binder_) {
		tmp.swap(cached_async_write_binder_);
	}
	if(!tmp) {
		tmp = new connection::async_write_binder();
	}
	tmp->conn = self();
	tmp->h = h;
	tmp->complete_response = complete_response;
	// ready

	booster::system::error_code e;
	if(response.flush_async_chunk(e)!=0 || !has_pending()) {
		get_io_service().post(tmp,e);
		return;
	}
	async_write(booster::aio::const_buffer(),false,tmp);
}

bool connection::has_pending()
{
	return !pending_output_.empty();
}

void connection::append_pending(booster::aio::const_buffer const &new_data)
{
	size_t pos = pending_output_.size();
	pending_output_.resize(pending_output_.size() + new_data.bytes_count());
	std::pair<booster::aio::const_buffer::entry const *,size_t> packets = new_data.get();
	for(size_t i=0;i<packets.second;i++) {
		memcpy(&pending_output_[pos],packets.first[i].ptr,packets.first[i].size);
		pos += packets.first[i].size;
	}
}

bool connection::write(booster::aio::const_buffer const &buf,bool eof,booster::system::error_code &e)
{
	booster::aio::const_buffer new_data = format_output(buf,eof,e);
	if(e) return  false;
	booster::aio::const_buffer output = pending_output_.empty() ? new_data : (booster::aio::buffer(pending_output_) + new_data);
	if(output.empty())
		return true;
	socket().set_non_blocking_if_needed(false,e);
	if(e) return false;

	bool r=write_to_socket(output,e);
	pending_output_.clear();
	return r;
}

bool connection::write_to_socket(booster::aio::const_buffer const &in,booster::system::error_code &e)
{
	return socket().write(in,e) == in.bytes_count();
}

bool connection::nonblocking_write(booster::aio::const_buffer const &buf,bool eof,booster::system::error_code &e)
{
	booster::aio::const_buffer new_data = format_output(buf,eof,e);
	if(e) return  false;
	booster::aio::const_buffer output = pending_output_.empty() ? new_data : (booster::aio::buffer(pending_output_) + new_data);
	if(output.empty())
		return true;
	
	socket().set_non_blocking_if_needed(true,e);
	if(e) return false;

	size_t n = socket().write_some(output,e);
	if(n == output.bytes_count()) {
		pending_output_.clear();
		return true;
	}
	if(n == 0) {
		append_pending(new_data);
	}
	else {
		std::vector<char> tmp;
		pending_output_.swap(tmp); 
		// after swapping output still points to a valid buffer
		append_pending(output + n);
	}
	if(e && socket().would_block(e)) {
		e=booster::system::error_code();
		return false;
	}
	return false;
}

struct connection::async_write_handler : public booster::callable<void(booster::system::error_code const &e)>
{
	typedef booster::shared_ptr<cppcms::impl::cgi::connection> conn_type;
	typedef booster::intrusive_ptr< booster::callable<void(booster::system::error_code const &)> > self_type;
	std::vector<char> data;
	booster::aio::const_buffer output;
	handler h;
	conn_type conn;

	async_write_handler(conn_type const &c,std::vector<char> &d,handler const &hin) :
		h(hin),
		conn(c)
	{
		data.swap(d);
		output = booster::aio::buffer(data);
	}

	virtual void operator()(booster::system::error_code const &ein)
	{
		if(ein) { h(ein); return; }
		booster::system::error_code e;
		conn->socket().set_non_blocking_if_needed(true,e);
		size_t n = conn->socket().write_some(output,e);
		output += n;
		if(n!=0) {
			conn->on_async_write_progress(output.empty());
		}
		if(output.empty()) {
			h(e);
			return;
		}
		if(e && !booster::aio::basic_io_device::would_block(e)) {
			h(e);
			return;
		}
		conn->socket().on_writeable(self_type(this));
	}
};

void connection::async_write(booster::aio::const_buffer const &buf,bool eof,handler const &h)
{
	booster::system::error_code e;
	if(nonblocking_write(buf,eof,e) || e) {
		get_io_service().post(h,e);
		return;
	}
	on_async_write_start();
	async_write_handler::self_type p(new async_write_handler(self(),pending_output_,h));
	socket().on_writeable(p);
}



struct connection::reader {
	reader(connection *C,io_handler const &H,size_t S,char *P) : h(H), s(S), p(P),conn(C)
	{
		done=0;
	}
	io_handler h;
	size_t s;
	size_t done;
	char *p;
	connection *conn;
	void operator() (booster::system::error_code const &e=booster::system::error_code(),size_t read = 0)
	{
		if(e) {
			h(e,done+read);
			return;
		}
		s-=read;
		p+=read;
		done+=read;
		if(s==0)
			h(booster::system::error_code(),done);
		else
			conn->async_read_some(p,s,*this);
	}
};

std::string connection::format_xcgi_response_headers(cppcms::impl::response_headers &hdr)
{
	cppcms::impl::response_headers::string_buffer_wrapper buf(hdr.estimate_size());
	if(service().cached_settings().service.generate_http_headers)
		hdr.format_http_headers(buf,"1.0",true);
	else
		hdr.format_cgi_headers(buf,true);
	return std::move(buf.data());
}

void connection::async_read(void *p,size_t s,io_handler const &h)
{
	reader r(this,h,s,(char*)p);
	r();
}

connection::connection(cppcms::service &srv) :
	service_(&srv),
    error_state_(false)
{
}

connection::~connection()
{
}


} // cgi
} // impl
} // cppcms
