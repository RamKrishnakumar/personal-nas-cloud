
NestDrive HTTPS foundation — self-signed setup

Files to change now:
1. src/core/AppConfig/AppConfig.h
2. src/core/AppConfig/AppConfig.cpp
3. src/core/FileServer/FileServer.h
4. src/core/FileServer/FileServer.cpp
5. src/ui/MainWindow.cpp
6. config.ini / config.ini.example

What Step 1 does:
- Adds config values for HTTPS and TLS file paths.
- Adds FileServer support for HTTPS mode and certificate loading.
- Updates MainWindow so the status URL shows https:// and the app refuses to start if TLS files are missing.

What still remains after Step 1:
- Actual TLS socket handling inside FileServer.cpp
- Login routes, password storage, sessions, and roles in later steps

Self-signed certificate files to place:
- Put them next to your exe using:
  <build-folder>/certs/server.crt
  <build-folder>/certs/server.key

Quick OpenSSL command:
openssl req -x509 -nodes -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -subj "/CN=nestdrive.ddns.net"

Then move them into:
<build-folder>/certs/

Important note:
Your current FileServer.cpp must still be updated to use QSslSocket for accepted connections. The header and config changes here prepare that foundation, but the socket upgrade code is the next exact implementation step.
