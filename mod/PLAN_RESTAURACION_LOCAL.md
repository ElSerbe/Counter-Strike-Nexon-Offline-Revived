# Plan: restauración local de Counter-Strike Nexon

Fuente: objetivo del proyecto indicado por el usuario (2026-10-14).

## Objetivo
Juego completamente funcional sin servicios de Nexon: arranque sin Internet, menú/hub, partidas locales con bots en todos los modos que el cliente pueda ejecutar, perfil y economía locales, y en una fase posterior una capa multijugador propia (cliente-servidor o P2P) independiente de la infraestructura original.

## Mapa de dependencias online → sustituto local

| Dependencia original | Qué hace | Sustituto local | Estado |
|---|---|---|---|
| API HTTPS `*.execute-api.ap-northeast-1` (login, colección de ajustes, lista de servidores) | Autenticación y datos de servicio | Servicio HTTPS local en `127.0.0.1:443`; el cliente acepta certificado propio porque WinHTTP usa `IGNORE_UNKNOWN_CA/CN/DATE` (0x3300) | Stub activo; responde `{}`. Falta el formato real de cada respuesta |
| `*.gamelog.nexon.com` / analítica (HTTP plano) | Telemetría | Stub HTTP local que responde 200 | Stub activo. Telemetría anulada (`NexonAnalytics32.dll` DllMain) |
| Servidor de juego/lobby TCP (30002 por defecto) y sondeo UDP `ALIVE` | Salas, partidas, perfil, tienda | `CSNZ_Server` (código abierto, GPL-3.0) con perfil en SQLite | Compilado y en ejecución en `127.0.0.1:30002`; el cliente conecta pero el handshake aún se corta |
| Lista de servidores en memoria (16 bytes por entrada) | Destinos de conexión | Inyectar `127.0.0.1:30002` cuando la API no devuelve nada | Pendiente: ruta de llenado en `0x2796A00` y funciones que llama |
| Verificación de cliente (`ClientRegistry.blob`, hash de integridad) | Anti-trampas | Parches de `hw.dll` / `NexonAnalytics32.dll` documentados en `apply_patches.py` | Parches aplicados |

## Perfil y economía locales
- Perfil (nombre, nivel, EXP, estadísticas, inventario, equipamiento, configuración, monedas, progreso, desbloqueos): almacenado por `CSNZ_Server` en SQLite. Verificar que cubre todos los campos.
- Tienda: solo moneda de juego. Revisar `shop.md` / `Shop.json` de `CSNZ_Server` y eliminar cualquier precio en moneda premium.

## Fases
1. **Arranque sin conexión hasta el hub.** Ya cumplido: el juego llega al menú principal con el stub HTTP/HTTPS y los parches.
2. **Conexión al servidor local.** Completar la lista de servidores (`0x2796A00`), responder `ALIVE` por UDP en 30002 y resolver por qué `CSNZ_Server` corta la conexión tras `~SERVERCONNECTED`.
3. **Perfil y tienda locales.** Login y datos de perfil desde SQLite; tienda con moneda interna.
4. **Partidas locales con bots.** Cargar mapas y modos del cliente contra `CSNZ_Server` en modo local.
5. **Multijugador propio.** Nueva capa de red (cliente-servidor o P2P), independiente del protocolo original.

## Riesgos y decisiones abiertas
- `CSNZ_Server` es GPL-3.0. Si el paquete del mod distribuye su binario, hay que cumplir la licencia (código fuente disponible).
- El protocolo original (API y servidor de juego) está documentado solo en parte; cada respuesta nueva requiere análisis con REA/Ghidra.
- El ejecutable del juego pide `RequireAdministrator`; sin cambiarlo, cada arranque necesita aprobación UAC.
