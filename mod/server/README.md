# Servidor local (CSNZ_Server)

Servidor maestro local con el que el cliente parcheado inicia sesión. Basado en
[JusicP/CSNZ_Server](https://github.com/JusicP/CSNZ_Server) (GPL-3.0, ver `CSNZ_Server/LICENSE`).

## Arrancar

```
run_server.cmd
```

Escucha en `127.0.0.1:30002` con SSL. El log va a `server.log` y a `CSNZ_Server/bin/Logs/`.

## Qué tiene de especial esta copia

- **Base:** commit indicado en `patches/BASE_COMMIT.txt`.
- **Cambios de código:** `patches/csnz_server-local.patch`
  - Arreglos de compilación con WinLibs (GCC 16).
  - SSL: el handshake ya no se hace en bucle dentro de `CTCPServer::Accept()`. El cliente abre
    dos conexiones y solo hace TLS en la segunda, así que el bucle bloqueaba el servidor.
    Ahora `wolfSSL_recv` completa el handshake cuando llegan datos, y las lecturas
    `WANT_READ/WANT_WRITE` se reintentan en vez de desconectar (`IExtendedSocket::WouldBlock`).
- **Configuración (`bin/ServerConfig.json`):**
  - `"SSL": true`, con `Data/Certs/server-cert.pem` y `server-key.pem` (autofirmados).
  - `Metadata`: el conjunto original del servidor (41 activos), igual que
    `ServerConfig.metadata-on.json`. `ServerConfig.maps-only.json` guarda la prueba con solo
    mapas y modos.
- **kuba-zip sin ZIP64:** `patches/kuba-zip-no-zip64.patch` (submódulo `src/thirdparty/zip`,
  commit en `BASE_COMMIT.txt`). Los metadatos se envían comprimidos y el cliente los abre con
  XUnzip (ZipUtils), que no entiende ZIP64. Con ZIP64 el cliente fallaba en un `memmove`
  al leer el directorio central y se cerraba nada más hacer login.
- **DLLs junto al exe:** `libstdc++-6`, `libgcc_s_seh-1`, `libwinpthread-1` de WinLibs.
  La `libwinpthread-1.dll` de Rust (GNU), que está antes en el PATH, es antigua y no tiene
  `nanosleep64` ni `clock_gettime64`.
- **Cuentas:** `bin/UserDatabase.db3` (cuenta de prueba `admin`).

## Recompilar

El código se clona en `mod/server/src-repo` (ignorado por Git; el parche es la copia que se guarda).

1. `git clone https://github.com/JusicP/CSNZ_Server src-repo`, `git checkout` del commit base y
   `git submodule update --init --recursive`. Si git falla con "SSL certificate problem", añade
   `-c http.sslBackend=schannel` (usa los certificados de Windows).
2. `git apply patches/csnz_server-local.patch`. Los submódulos no los mueve `git apply`: pon cada uno en el
   commit que dice el parche (líneas `+Subproject commit ...`). Dentro de `src/thirdparty/zip`,
   `git apply ../../../../patches/kuba-zip-no-zip64.patch`.
3. Con WinLibs en el PATH: `cmake -S src -B src/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release` y
   `cmake --build src/build --target PROJECTNAME` (el target `test` no compila y no hace falta). Sale en
   `src-repo/bin/CSNZ_Server.exe`; cópialo a `CSNZ_Server/bin` (y las DLL de WinLibs si cambia de versión).
   Compila con GCC 16 (el parche ya incluye los arreglos para él).
4. Para regenerar el parche tras cambiar el código: `git add -N` de los archivos nuevos y
   `git diff > ../patches/csnz_server-local.patch`.
