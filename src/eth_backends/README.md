== Ethernet support and emulation backends ==

eth_backends.h: eth_api_t enum, eth_backend_t type definitions. eth_api_t
  enumerates the different backends. eth_backend_t holds the state related to
  each backend, as well as the API function pointers.

eth_dispatch.c, eth_dispatch.h: Backend read and write functions.
