# Security policy

This project is an experimental, opt-in face authentication method for personal KDE lock screens. Its active infrared check measures scene illumination. Presentation-attack resistance and false-match rates have yet to be independently validated.

Use the dedicated `kde-face` PAM service and keep password authentication available. The supported installation path leaves login and privilege-elevation policies in place. Hardware support varies by model and kernel. The README lists configurations verified on a device.

Enrollment requires administrator authorization. The application stores face embeddings locally under `/etc/surface-ir-hello/faces/`. Treat them and any manually exported camera frames as sensitive data.

## Report a vulnerability

Use GitHub private vulnerability reporting when available. Otherwise, open an issue asking for a private contact method and share the details through that channel after a maintainer responds. Include the affected commit or version and the relevant system configuration. Redact account names, credentials, template data, and camera images.
