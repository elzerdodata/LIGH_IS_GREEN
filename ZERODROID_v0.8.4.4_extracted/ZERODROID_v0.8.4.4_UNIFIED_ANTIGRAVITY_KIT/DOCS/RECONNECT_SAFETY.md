# Seguridad de Reconnect Same Session

La ruta de reconexión llama `disconnect_for_reconnect()`, que ejecuta shutdown
con `preserveRemoteSession=true`.

En esta ruta no deben enviarse:

```text
settings/terminating
gateway /api/hangup
API stop/dequeue de la sesión remota
```

Antes de cerrar el transporte deben liberarse clics sintéticos y botones. Luego
se destruyen video/audio/control locales y se intenta iniciar nuevamente el
mismo appId y resolución.

Limitación: conservar la VM no garantiza que Boosteroid vaya a reutilizarla. La
conducta final depende del servicio. Por eso:

- botón con confirmación;
- estado experimental;
- prueba separada;
- no usar como único mecanismo de salida.
