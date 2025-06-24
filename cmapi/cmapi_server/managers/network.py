import errno
import fcntl
import logging
import socket
import struct
from ipaddress import ip_address
from typing import Optional


class NetworkManager:
    @classmethod
    def get_ip_version(cls, ip_addr: str) -> int:
        """Get version of a given IP address.

        :param ip_addr: IP to get version
        :type ip_addr: str
        :return: version of a given IP
        :rtype: int
        """
        return ip_address(ip_addr).version

    @classmethod
    def get_ip_address_by_nic(cls, ifname: str) -> str:
        """Get ip address for nic.

        :param ifname: network intarface name
        :type ifname: str
        :return: ip address
        :rtype: str
        """
        # doesn't work on windows,
        # OpenBSD and probably doesn't on FreeBSD/pfSense either
        ip_addr: str  = ''
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            ip_addr = socket.inet_ntoa(
                fcntl.ioctl(
                    s.fileno(),
                    0x8915,  # SIOCGIFADDR "socket ioctl get interface address"
                    struct.pack('256s', bytes(ifname[:15], 'utf-8'))
                )[20:24]
            )
        except OSError as exc:
            if exc.errno == errno.ENODEV:
                logging.error(
                    f'Interface "{ifname}" doesn\'t exist.'
                )
            else:
                logging.error(
                    f'Unknown OSError code while getting IP for an "{ifname}"',
                    exc_info=True
                )
        except Exception:
            logging.error(
                (
                    'Unknown exception while getting IP address of an '
                    f'{ifname} interface',
                ),
                exc_info=True
            )
        finally:
            s.close()
        return ip_addr

    @classmethod
    def get_ips(
        cls, ignore_loopback: bool = False, only_ipv4: bool = True
    ) -> list[str]:
        """Getting all ips for all existing interfaces.

        :param ignore_loopback: ignore loopback addresses, defaults to False
        :type ignore_loopback: bool, optional
        :return: ip addresses for all node interfaces
        :rtype: list
        """
        ext_ips: list = []
        loopback_ips: list = []
        for _, nic_name in socket.if_nameindex():
            ip_addr = cls.get_ip_address_by_nic(nic_name)
            # skip IPv6 if needed
            if only_ipv4 and cls.get_ip_version(ip_addr) != 4:
                continue
            if ip_address(ip_addr).is_loopback:
                loopback_ips.append(ip_addr)
            else:
                ext_ips.append(ip_addr)
        result = [*ext_ips, *loopback_ips] if not ignore_loopback else ext_ips
        return result

    @classmethod
    def get_hostname(cls, ip_addr: str) -> Optional[str]:
        """Get hostname for a given IP address.

        :param ip_addr: IP address to get hostname
        :type ip_addr: str
        :return: hostname if exist else None
        :rtype: Optional[str]
        """
        try:
            hostnames = socket.gethostbyaddr(ip_addr)
            return hostnames[0]
        except socket.herror:
            logging.error(f'No hostname found for address: "{ip_addr}"')
            return None
