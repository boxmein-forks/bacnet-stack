BACnet Tools are binary demo application command line
utilities that use BACnet/IP to perform a variety of
BACnet services. Some tools use BACnet WhoIs to bind to
devices, but can also use a static binding file address_cache.

Most of the tools have help (--help option), and use
environment variables to configure the datalink.

The Client Tools use WhoIs to bind to target devices.
The WhoIs can be eliminated by using the address_cache
file, which is read by each client tool from the current
working directory.  Having the device address from the
address_cache file will greatly improve the throughput
and speed of the client tools. The address_cache file
can be generated using the standard output of the bacwi tool.

EXAMPLE:
bacwi -1 > address_cache

Client Tools
------------
bacarf - BACnet AtomicReadFile service
bacawf - BACnet AtomicWriteFile service
bacdcc - BACnet DeviceCommunicationControl service
bacepics - BACnet EPICS for Device objects and properties
bacrd - BACnet ReinitializeDevice service
bacrp - BACnet ReadProperty service
bacrpm - BACnet ReadPropertyMultiple service
bacwpm - BACnet WritePropertyMultiple service
bacscov - BACnet SubscribeCOV service
bacts - BACnet TimeSynchronization service
bacucov - BACnet UnconfirmedChangeOfValue service
bacupt - BACnet UnconfirmedPrivateTransfer service
bacwh - BACnet WhoHas service
bacwi - BACnet WhoIs service
bacwp - BACnet WriteProperty service
bacale - BACnet AddListElement service
bacrle - BACnet RemoveListElement service
bacackalarm - BACnet AcknowledgeAlarm service
bacevent - BACnet ConfirmedEventNotification service
bacuevent - BACnet UnconfirmedEventNotification service
baciam - BACnet I-Am service
bacge - BACnet GetEventInformation service
bacco - BACnet CreateObject service
bacdo - BACnet DeleteObject service
bacrr - BACnet ReadRange service

Client Error/Abort
-----------------
bacerror - BACnet Error message
bacabort - BACnet Abort message

Server Tools
------------
bacserv - BACnet Device Simulator with example objects
bacpoll - BACnet Device with client polling example

Router Tools
------------
baciamr - BACnet I-Am-Router to Network message
bacinitr - BACnet Initialize Router message
bacwir - BACnet Who-Is Router to Network message
bacnni - BACnet Network-Number-Is message
bacwinn - BACnet Who-Is-Network-Number message

MS/TP Tools
------------------
mstpcap - a tool that is used for capturing MS/TP traffic
from an RS-485 serial adapter and saving the packets
in a file for viewing by Wireshark.

mstpcrc - calculates Header CRC or Data CRC for ascii hex or decimal input.
Optionally takes the input and saves it to a PCAP format file for viewing
in Wireshark.

BACnet/IP Tools
---------------
bacwbdt - BACnet/IP Write-Distribution-Table message
bacrbdt - BACnet/IP Read-Distribution-Table message
bacrfdt - BACnet/IP Read-Foreign-Device-Table message

Environment Variables
---------------------
BACNET_APDU_TIMEOUT - set this value in milliseconds to change
    the APDU timeout.  APDU Timeout is how much time a client
    waits for a response from a BACnet device. Default is 3000ms.

BACNET_APDU_RETRIES - indicate the maximum number of times that
    an APDU shall be retransmitted.

BACNET_IFACE - set this value to dotted IP address (Windows) of
    the interface (see ipconfig command on Windows) for which you
    want to bind.  On Linux, set this to the /dev interface
    (i.e. eth0, arc0).  Default is eth0 on Linux, and the default
    interface on Windows.  Hence, if there is only a single network
    interface on Windows, the applications will choose it, and this
    setting will not be needed.

BACNET_IP_PORT - UDP/IP port number (0..65534) used for BACnet/IP
    communications.  Default is 47808 (0xBAC0).

BACNET_BBMD_PORT - UDP/IP port number (0..65534) used for Foreign
    Device Registration.  Defaults to 47808 (0xBAC0).

BACNET_BBMD_TIMETOLIVE - number of seconds used in Foreign Device
    Registration (0..65535). Defaults to 60000 seconds.

BACNET_BBMD_ADDRESS - dotted IPv4 address of the BBMD or Foreign Device
    Registrar.

BACNET_BDT_ADDR_1 - dotted IPv4 address of the BBMD table entry 1..128
BACNET_BDT_PORT_1 - UDP port of the BBMD table entry 1..128 (optional)
BACNET_BDT_MASK_1 - dotted IPv4 mask of the BBMD table
    entry 1..128 (optional)

BACNET_IP_NAT_ADDR - dotted IPv4 address of the public facing router

BACNET_IP_BROADCAST_BIND_ADDR - dotted IPv4 address to bind broadcasts

When the tools are compiled to use MS/TP datalink, the following
environment variables are used:

BACNET_MAX_INFO_FRAMES - BACnet MS/TP max-info-frames parameter.
  Defaults to 127.

BACNET_MAX_MASTER -  BACnet MS/TP max-master parameter.
  Defaults to 127.

BACNET_MSTP_BAUD - BACnet MS/TP baud rate.
  Defaults to 38400.

BACNET_MSTP_MAC - BACnet MS/TP MAC address.
  Defaults to 127.

BACNET_IFACE - interface to use for the MS/TP datalink layer
  For Linux, this is something like /dev/ttyS0 or /dev/ttyUSB0
  For Windows, this is something like COM4 or COM23
  Defaults to /dev/ttyUSB0 on Linux and COM4 on Windows
  and /dev/cu.usbserial-7 on macOS.

Example Usage
-------------
You can communicate with the virtual BACnet Device by using the other BACnet
command line tools.  If you are using the same PC, you can use BBMD/FD
(Foreign Device registration) to do this - use the bvlc script.  You can
monitor the interaction and bytes on the wire using Wireshark.  Here is
an example usage for Window and for Linux.

Windows
-------
The BACnet tools are used from the Command Prompt, or CMD.EXE.
From the command prompt window, start the simulated BACnet device:
c:\> bacserv 1234

From another command prompt window, use ipconfig to determine the
network interface IP address that bacserv is using:
c:\> ipconfig

Use the default IP address to configure the BBMD and Foreign Device
environment variables:
c:\> bvlc.bat 192.168.0.42

bvlc.bat batch file configures environment variables to use BACnet/IP
port 47809 for any subsequent BACnet tools run from that command prompt window,
and enables the BBMD Foreign Device Registration.

Perform a device discovery:
c:\> bacwi -1

Read all the required properties from the Device 1234 and display their values:
c:\> bacepics -v 1234

Read the Object_Identifier property from the Device 1234:
c:\> bacrp 1234 8 1234 75

Write 100.0 (REAL=4 datatype) to Device 1234 Analog Output (1) One (1)
at priority 16 with no index (-1).
c:\> bacwp 1234 1 1 85 16 -1 4 100.0

Each tool has help:
c:\> bacrp --help

Linux
-----
To use the tools from the command line, you need to use the path to the command,
or include the path in your PATH environment variable.  The dot "." means current
directory.  The "/" is used to separate directories. "./" means the path starts
from the current directory.

When the tools are built from the Makefile, they are copied to the bin/ directory.
So from the root of the project you could run the tools like this using a terminal
window:
$ make clean all
$ ./bin/bacserv 1234

In another terminal window use ifconfig to determine the network interface IP
address that bacserv is using:
$ ifconfig

Use that address (likely from eth0) to configure the BBMD and Foreign Device
environment variables:
$./bin/bvlc.sh 192.168.0.42
bvlc.sh script configures environment variables to use BACnet/IP
port 47809 for any subsequent BACnet tools run from that shell,
and enables the BBMD Foreign Device Registration.

Perform a device discovery:
$ ./bin/bacwi -1

Read all the required properties from the Device 1234 and display their values:
$ ./bin/bacepics -v 1234

Read the Object_Identifier property from the Device 1234:
$ ./bin/bacrp 1234 8 1234 75

Write 100.0 (REAL=4 datatype) to Device 1234 Analog Output (1) One (1)
at priority 16 with no index (-1).
$ ./bin/bacwp 1234 1 1 85 16 -1 4 100.0

Each tool has help:
$ ./bin/bacrp --help

Source Code
-----------
The source code and makefiles for the bacnet-tools is included in the
BACnet Protocol Stack library and can be found at:
http://bacnet.sourceforge.net/

The bacnet-tools source is located in bacnet-stack/apps/project where:
bacarf - bacnet-stack/apps/readfile
bacawf - bacnet-stack/apps/writefile
bacdcc - bacnet-stack/apps/dcc
bacepics - bacnet-stack/apps/epics
bacrd - bacnet-stack/apps/reinit
bacrp - bacnet-stack/apps/readprop
bacrpm - bacnet-stack/apps/readpropm
bacscov - bacnet-stack/apps/scov
bacts - bacnet-stack/apps/timesync
bacucov - bacnet-stack/apps/ucov
bacupt - bacnet-stack/apps/uptransfer
bacwh - bacnet-stack/apps/whohas
bacwi - bacnet-stack/apps/whois
bacwp - bacnet-stack/apps/writeprop
bacserv - bacnet-stack/apps/server
etc.
