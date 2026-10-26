# Counter-Strike Nexon offline

## Jugar

Doble clic en **CSNZ Offline** (en el escritorio o en esta carpeta). Windows pedirá permiso de
administrador: el ejecutable del juego lo exige.

El lanzador (`CSNZ_Offline.ps1`):

1. Comprueba que el juego no está abierto y que el bloque de CSNZ sigue en el archivo `hosts`.
2. Aplica los parches del cliente con `tools\apply_patches.py`. Si Steam actualizó el juego y los
   archivos ya no coinciden, avisa y no arranca.
3. Arranca el servidor (`server\`, puerto 30002) si no estaba ya en marcha.
4. Abre el juego y, mientras está abierto, anota en `logs\network.log` cualquier conexión del juego
   a una dirección que no sea local (debería quedar vacío).
5. Al cerrar el juego apaga el servidor si lo arrancó él.

Dentro del juego aparece la ventana **Choose your character**: elige un personaje, crea uno nuevo o
borra uno (hay que escribir su nombre para confirmarlo). Cancel cierra el juego. Si la ventana se
cierra de otra forma, vuelve a salir sola o con `/character` en el chat.

## Telemetría y anticheat

- La analítica de Nexon (`NexonAnalytics32.dll`) no se carga.
- `nxgsm.dll` (registros de etapa) y `CrashReporter.dll` (informes de fallos y su configuración en AWS)
  tienen sus funciones parcheadas para volver sin hacer nada.
- El anticheat (`NGClient.aes`, que arrancaba BlackCipher y enviaba registros a `psm-log.ngs.nexon.com`)
  no se inicia ni se le llama, y su DllMain no ejecuta nada.
- Los banners de promoción del lobby (S3 de Nexon) y el servidor de vóxeles apuntan a 127.0.0.1.

El bloque del archivo `hosts` sigue como segunda barrera, y `logs\network.log` muestra cualquier
conexión externa que quede.

## Dónde está cada cosa

- `logs\` — log del lanzador (`launcher.log`) y de conexiones externas del juego (`network.log`).
- `server\` — servidor local (ver `server\README.md`); las cuentas están en
  `server\CSNZ_Server\bin\UserDatabase.db3`.
- `stubs\` — servidores web locales que se usaron para investigar las peticiones del juego; ya no se arrancan.
- `tools\apply_patches.py` — parches del cliente (`apply` / `revert`).
- `backup\` — copia original de `Bin` y del archivo `hosts`.
