#include "tcp_socket.hpp"

#include <cstring>

#if M_OS == M_OS_LINUX || M_OS == M_OS_MACOSX || M_OS == M_OS_UNIX
#	include <netinet/in.h>
#endif

using namespace setka;

void tcp_socket::open(const address& ip, bool disableNaggle){
	if(*this){
		throw std::logic_error("tcp_socket::Open(): socket already opened");
	}

#if M_OS == M_OS_WINDOWS
	this->create_event_for_waitable();
#endif

	this->sock = ::socket(
			ip.host.is_v4() ? PF_INET : PF_INET6,
			SOCK_STREAM,
			0
		);
	if(this->sock == invalid_socket){
#if M_OS == M_OS_WINDOWS
		int error_code = WSAGetLastError();
		this->close_event_for_waitable();
#else
		int error_code = errno;
#endif
		throw std::system_error(error_code, std::generic_category(), "couldn't create socket, socket() failed");
	}

	// disable Naggle algorithm if required
	if(disableNaggle){
		this->disable_naggle();
	}

	this->set_nonblocking_mode();

	this->readiness_flags.clear();

	// connecting to remote host
	sockaddr_storage sockAddr;
	
	if(ip.host.is_v4()){
		sockaddr_in &sa = reinterpret_cast<sockaddr_in&>(sockAddr);
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(ip.host.get_v4());
		sa.sin_port = htons(ip.port);
	}else{
		sockaddr_in6 &sa = reinterpret_cast<sockaddr_in6&>(sockAddr);
		memset(&sa, 0, sizeof(sa));
		sa.sin6_family = AF_INET6;
#if M_OS == M_OS_MACOSX || M_OS == M_OS_WINDOWS || (M_OS == M_OS_LINUX && M_OS_NAME == M_OS_NAME_ANDROID)
		sa.sin6_addr.s6_addr[0] = ip.host.quad[0] >> 24;
		sa.sin6_addr.s6_addr[1] = (ip.host.quad[0] >> 16) & 0xff;
		sa.sin6_addr.s6_addr[2] = (ip.host.quad[0] >> 8) & 0xff;
		sa.sin6_addr.s6_addr[3] = ip.host.quad[0] & 0xff;
		sa.sin6_addr.s6_addr[4] = ip.host.quad[1] >> 24;
		sa.sin6_addr.s6_addr[5] = (ip.host.quad[1] >> 16) & 0xff;
		sa.sin6_addr.s6_addr[6] = (ip.host.quad[1] >> 8) & 0xff;
		sa.sin6_addr.s6_addr[7] = ip.host.quad[1] & 0xff;
		sa.sin6_addr.s6_addr[8] = ip.host.quad[2] >> 24;
		sa.sin6_addr.s6_addr[9] = (ip.host.quad[2] >> 16) & 0xff;
		sa.sin6_addr.s6_addr[10] = (ip.host.quad[2] >> 8) & 0xff;
		sa.sin6_addr.s6_addr[11] = ip.host.quad[2] & 0xff;
		sa.sin6_addr.s6_addr[12] = ip.host.quad[3] >> 24;
		sa.sin6_addr.s6_addr[13] = (ip.host.quad[3] >> 16) & 0xff;
		sa.sin6_addr.s6_addr[14] = (ip.host.quad[3] >> 8) & 0xff;
		sa.sin6_addr.s6_addr[15] = ip.host.quad[3] & 0xff;

#else
		sa.sin6_addr.__in6_u.__u6_addr32[0] = htonl(ip.host.quad[0]);
		sa.sin6_addr.__in6_u.__u6_addr32[1] = htonl(ip.host.quad[1]);
		sa.sin6_addr.__in6_u.__u6_addr32[2] = htonl(ip.host.quad[2]);
		sa.sin6_addr.__in6_u.__u6_addr32[3] = htonl(ip.host.quad[3]);
#endif
		sa.sin6_port = htons(ip.port);
	}

	// connect to the remote host
	if(connect(
			this->sock,
			reinterpret_cast<sockaddr *>(&sockAddr),
			ip.host.is_v4() ? sizeof(sockaddr_in) : sizeof(sockaddr_in6) // NOTE: on Mac OS for some reason the size should be exactly according to AF_INET/AF_INET6
		) == socket_error)
	{
#if M_OS == M_OS_WINDOWS
		int errorCode = WSAGetLastError();
#elif M_OS == M_OS_LINUX || M_OS == M_OS_MACOSX || M_OS == M_OS_UNIX
		int errorCode = errno;
#else
#	error "Unsupported OS"
#endif
		if(errorCode == error_interrupted){
			// do nothing, for non-blocking socket the connection request still should remain active
		}else if(errorCode == error_in_progress){
			// do nothing, this is not an error, we have non-blocking socket
		}else{
			this->close();

			throw std::system_error(errorCode, std::generic_category(), "could not connect to remote host, connect() failed");
		}
	}
}

size_t tcp_socket::send(const utki::span<uint8_t> buf){
	if(!*this){
		throw std::logic_error("tcp_socket::Send(): socket is not opened");
	}

	this->readiness_flags.clear(opros::ready::write);

#if M_OS == M_OS_WINDOWS
	int len;
#else
	ssize_t len;
#endif

	while(true){
		len = ::send(
				this->sock,
				reinterpret_cast<const char*>(&*buf.begin()),
				int(buf.size()),
				0
			);
		if(len == socket_error){
#if M_OS == M_OS_WINDOWS
			int errorCode = WSAGetLastError();
#else
			int errorCode = errno;
#endif
			if(errorCode == error_interrupted){
				continue;
			}else if(errorCode == error_again){
				// can't send more bytes, return 0 bytes sent
				len = 0;
			}else{
				throw std::system_error(errorCode, std::generic_category(), "could not send data over network, send() failed");
			}
		}
		break;
	}

	ASSERT(len >= 0)
	return size_t(len);
}

size_t tcp_socket::recieve(utki::span<uint8_t> buf){
	// the 'ready to read' flag shall be cleared even if this function fails to avoid subsequent
	// calls to recv() because it indicates that there's activity.
	// So, do it at the beginning of the function.
	this->readiness_flags.clear(opros::ready::read);

	if(!*this){
		throw std::logic_error("tcp_socket::Recv(): socket is not opened");
	}

#if M_OS == M_OS_WINDOWS
	int len;
#else
	ssize_t len;
#endif

	while(true){
		len = ::recv(
				this->sock,
				reinterpret_cast<char*>(&*buf.begin()),
				int(buf.size()),
				0
			);
		if(len == socket_error){
#if M_OS == M_OS_WINDOWS
			int errorCode = WSAGetLastError();
#else
			int errorCode = errno;
#endif

			if(errorCode == error_interrupted){
				continue;
			}else if(errorCode == error_again){
				// no data available, return 0 bytes received
				len = 0;
			}else{
				throw std::system_error(errorCode, std::generic_category(), "could not receive data form network, recv() failed");
			}
		}
		break;
	}

	ASSERT(len >= 0)
	return size_t(len);
}

namespace{
address make_ip_address(const sockaddr_storage& addr){
	if(addr.ss_family == AF_INET){
		const sockaddr_in &a = reinterpret_cast<const sockaddr_in&>(addr);
		return address(
			uint32_t(ntohl(a.sin_addr.s_addr)),
			uint16_t(ntohs(a.sin_port))
		);
	}else{
		ASSERT(addr.ss_family == AF_INET6)
		
		const sockaddr_in6 &a = reinterpret_cast<const sockaddr_in6&>(addr);
		
		return address(
				address::ip(
#if M_OS == M_OS_MACOSX || M_OS == M_OS_WINDOWS || (M_OS == M_OS_LINUX && M_OS_NAME == M_OS_NAME_ANDROID)
						(uint32_t(a.sin6_addr.s6_addr[0]) << 24) | (uint32_t(a.sin6_addr.s6_addr[1]) << 16) | (uint32_t(a.sin6_addr.s6_addr[2]) << 8) | uint32_t(a.sin6_addr.s6_addr[3]),
						(uint32_t(a.sin6_addr.s6_addr[4]) << 24) | (uint32_t(a.sin6_addr.s6_addr[5]) << 16) | (uint32_t(a.sin6_addr.s6_addr[6]) << 8) | uint32_t(a.sin6_addr.s6_addr[7]),
						(uint32_t(a.sin6_addr.s6_addr[8]) << 24) | (uint32_t(a.sin6_addr.s6_addr[9]) << 16) | (uint32_t(a.sin6_addr.s6_addr[10]) << 8) | uint32_t(a.sin6_addr.s6_addr[11]),
						(uint32_t(a.sin6_addr.s6_addr[12]) << 24) | (uint32_t(a.sin6_addr.s6_addr[13]) << 16) | (uint32_t(a.sin6_addr.s6_addr[14]) << 8) | uint32_t(a.sin6_addr.s6_addr[15])
#else
						uint32_t(ntohl(a.sin6_addr.__in6_u.__u6_addr32[0])),
						uint32_t(ntohl(a.sin6_addr.__in6_u.__u6_addr32[1])),
						uint32_t(ntohl(a.sin6_addr.__in6_u.__u6_addr32[2])),
						uint32_t(ntohl(a.sin6_addr.__in6_u.__u6_addr32[3]))
#endif
					),
				uint16_t(ntohs(a.sin6_port))
			);
	}
}
}

address tcp_socket::get_local_address(){
	if(!*this){
		throw std::logic_error("Socket::GetLocalAddress(): socket is not valid");
	}

	sockaddr_storage addr;

#if M_OS == M_OS_WINDOWS
	int len = sizeof(addr);
#else
	socklen_t len = sizeof(addr);
#endif

	if(getsockname(this->sock, reinterpret_cast<sockaddr*>(&addr), &len) == socket_error){
#if M_OS == M_OS_WINDOWS
		int error_code = WSAGetLastError();
#else
		int error_code = errno;
#endif
		throw std::system_error(error_code, std::generic_category(), "could not get local address, getsockname() failed");
	}	

	return make_ip_address(addr);
}

address tcp_socket::get_remote_address(){
	if(!*this){
		throw std::logic_error("tcp_socket::GetRemoteAddress(): socket is not valid");
	}

	sockaddr_storage addr;

#if M_OS == M_OS_WINDOWS
	int len = sizeof(addr);
#else
	socklen_t len = sizeof(addr);
#endif

	if(getpeername(this->sock, reinterpret_cast<sockaddr*>(&addr), &len) == socket_error){
#if M_OS == M_WINDOWS
		int error_code = WSAGetLastError();
#else
		int error_code = errno;
#endif
		throw std::system_error(error_code, std::generic_category(), "could not get remote address, getpeername() failed");
	}

	return make_ip_address(addr);
}

#if M_OS == M_OS_WINDOWS
void tcp_socket::set_waiting_flags(utki::flags<opros::ready> waiting_flags){
	long flags = FD_CLOSE;
	if(waiting_flags.get(opros::ready::read)){
		flags |= FD_READ;
		// NOTE: since it is not a tcp_server_socket, FD_ACCEPT is not needed here.
	}
	if(waiting_flags.get(opros::ready::write)){
		flags |= FD_WRITE | FD_CONNECT;
	}
	this->set_waiting_events_for_windows(flags);
}
#endif
