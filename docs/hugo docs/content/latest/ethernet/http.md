---
title: "HTTP Requests"
description: "Perform HTTP GET requests from the command line."
weight: 20
---

Make HTTP requests to web servers and APIs directly from GhostESP.

## Basic HTTP GET

Fetch content from a URL:

```
ethhttp https://<url> <lines_to_show|all>
```

**Parameters**:
- `<url>` — The full URL (supports http:// and https://)
- `<lines_to_show>` — Number of response lines to display, or `all` for complete response

**Examples**:

Fetch the first 10 lines of a webpage:
```
ethhttp https://example.com 10
```

Fetch the entire response:
```
ethhttp https://example.com all
```

Fetch from a specific path:
```
ethhttp https://api.example.com/status 5
```

## Use Cases

- **API Testing** — Query REST APIs and view responses
- **Web Scraping** — Retrieve specific content from websites
- **Service Verification** — Check if a web service is running and responding
- **Configuration Retrieval** — Fetch configuration files or data from remote servers

## Troubleshooting

- **Connection refused** — Ensure the server is running and accessible from your network
- **Timeout** — The server may be unreachable or slow to respond
- **SSL/TLS errors** — Some servers may require specific certificates; try http:// instead of https://
