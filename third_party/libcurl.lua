group("third_party")
project("libcurl")
  uuid("1ba7e608-5752-457c-8df0-c006c6e8b7fe")
  kind("StaticLib")
  language("C")
  defines({
    "BUILDING_LIBCURL",
    "HTTP_ONLY",
  })

  filter("platforms:Windows")
    links({
      "crypt32",
      "secur32",
    })
    defines({
      "USE_SCHANNEL",
      "USE_WINDOWS_SSPI",
    })

  filter("platforms:Linux")
    defines({
      "USE_OPENSSL",
      "HAVE_ARPA_INET_H",
      "HAVE_NETDB_H",
      "HAVE_NETINET_IN_H",
      "HAVE_SYS_SOCKET_H",
      "HAVE_UNISTD_H",
      "HAVE_FCNTL_H",
      "HAVE_FCNTL_O_NONBLOCK",
      "HAVE_FSETXATTR",
      "HAVE_RECV",
      "HAVE_SEND",
      "HAVE_SOCKET",
      "HAVE_SELECT",
      "HAVE_POLL_H",
      "HAVE_POLL_FINE",
      "HAVE_STRUCT_TIMEVAL",
      "HAVE_GETTIMEOFDAY",
      "HAVE_SIGACTION",
      "HAVE_SIGNAL_H",
      "HAVE_STRTOLL",
      "SIZEOF_CURL_OFF_T=8",
      "SIZEOF_LONG=8",
      "SIZEOF_SIZE_T=8",
      "SIZEOF_INT=4",
      "SIZEOF_SHORT=2",
      'CURL_OS="Linux"',
    })
    links({
      "ssl",
      "crypto",
    })

  filter {}

  filter {}

  includedirs({
    "libcurl/lib",
    "libcurl/include",

    -- "wolfssl",
    -- "wolfssl/src",
    -- "wolfssl/wolfssl",
    -- "wolfssl/wolfssl/openssl",
    -- "wolfssl/wolfssl/wolfcrypt",
  })
  files({
    "libcurl/lib/**.h",
    "libcurl/lib/**.c",
  })
