# Standalone Asio

This directory contains the official standalone Asio 1.36.0 headers from the
`asio-1-36-0` upstream tag: https://github.com/chriskohlhoff/asio

`IR_station` compiles these headers with `ASIO_STANDALONE`; no Asio DLL or
customer-side Asio package is required. The CMake install step deliberately
does not install this source dependency. See `LICENSE_1_0.txt` for the upstream
Boost Software License 1.0.
