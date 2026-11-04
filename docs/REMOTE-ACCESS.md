# Reaching NestDrive from outside the house

## Summary

There are two ways out, they suit different internet connections, and **NestDrive
measures which one applies rather than asking you to know.**

| | Who can view | Needs a port forwarded | Works behind provider NAT |
|---|---|---|---|
| **Tailscale** | devices signed into your tailnet | no | **yes** |
| **Public link** | anyone with the link | yes | no |

The choice lives in the control bar (*Auto / Tailscale / Public link / This network
only*) and in `config.ini` as `[Remote] method`. Auto prefers the public link where it
works, because that is the one that needs nothing installed on the viewing device.

It is one or the other, never both at once: the two need certificates for different
names, and Qt exposes no server-side SNI hook, so one listener can present exactly one
certificate.

This matters here because **the two connections in this household are one of each
kind** — the Airtel line is behind the provider's NAT and the BSNL line is not.

---

## 1. The measurement that decided this

Taken from the router's admin panel and from whatismyip.com, on the same connection,
at the same moment:

```
Connection type   PPPoE
Router WAN IP     172.40.42.138     ← what the router believes it has
Subnet mask       255.255.255.255
Public IP         111.91.226.113    ← what the internet actually sees
```

**The two differ.** That is the whole finding. There is a NAT between the router and
the internet, operated by the provider, which nobody on this end administers.

What that means concretely:

- A forwarding rule on the router maps `172.40.42.138:9090` to the laptop, and that
  part works correctly.
- But nothing on the internet can address `172.40.42.138`. It is not a routable
  destination.
- A connection to `111.91.226.113:9090` arrives at the provider's NAT, which holds no
  mapping for that port, and is dropped.

No router setting changes this. The port-forwarding page is working exactly as
designed; there is simply a second NAT behind it that the router cannot reach past.

`111.91.226.113` is also shared with other subscribers and changes on reconnect, so
pointing a DDNS hostname at it does not help either.

### Checking this on another connection

The usual advice is to look for an address in `100.64.0.0/10`, the range RFC 6598
reserves for this. That test gives a false negative here — this provider uses
`172.40.x`, which is neither RFC 1918 private space nor the reserved range.

**The reliable test is to compare the router's WAN IP against whatismyip.com.** If
they differ, port forwarding cannot work.

NestDrive now runs that comparison itself. It asks the router for its own WAN address
over **UPnP** and an external service for the address the internet sees, then compares
them — the direct question, rather than a guess from address ranges. The result is in
the **Remote access** dialog, along with the evidence.

Three outcomes:

- **Public IP** — the two agree. Forwarding a port will work.
- **Behind the provider's NAT** — they differ, or the public address is in the
  reserved range. Forwarding a port cannot work.
- **Not determined** — the router would not answer, which usually means UPnP is
  switched off in its settings, and the public address did not settle it alone. Make
  the comparison by hand once and record it as `[Remote] connectionType` in
  `config.ini`; NestDrive will use that whenever detection is inconclusive.

Detection runs at **every** startup and is trusted ahead of the stored value, because
the answer belongs to the connection and not to the machine. A laptop that moves
between these two providers would otherwise be told something true about the other
network.

### One consequence: DDNS stops publishing behind provider NAT

When the verdict is carrier-grade NAT, the DDNS updater is deliberately **not**
started. This is a refusal, not an optimisation: the address an external service
reports there belongs to the provider and is shared with other subscribers, so
publishing it would point this household's hostname at somebody else's connection.

### What this means for `DdnsUpdater`

The v0.4 DDNS updater is not broken. It does what it was built to do, and it is
useless on this connection through no fault of its own: there is no public address
for it to publish. It stays in the codebase and stays documented, because it is the
right answer the moment a public IP is available.

---

## 2. Why not libp2p

This was evaluated seriously and rejected on architecture, not maturity.

**The client is a browser, and we do not control it.** NestDrive serves media through
the browser's own HTTP stack: `<img src="/_thumb/…">`, `<video src="/_video/…">`, and
byte-range requests so a phone can seek inside a 4 GB file. A `<video>` element cannot
be pointed at a libp2p stream. Making it work would need js-libp2p running in the page
plus a Service Worker intercepting every fetch and proxying it over a WebRTC
datachannel — and a Service Worker requires a secure context, so HTTPS has to exist
first. The dependency runs in a circle.

There would also still be a hosted component to serve the page itself, and relay
infrastructure to run for NAT traversal.

libp2p is the right tool when both ends are your own application. Here one end is
Safari on somebody's phone.

### Would a native app change this?

Partly, and it is worth being precise about which part.

A native client on Windows, macOS, iOS and Android **does** remove the browser
constraint — that was the entire basis for rejecting libp2p above. Native clients can
speak QUIC, WebRTC, libp2p, WireGuard, anything.

What a native app does **not** remove is the need for a third party, because that need
comes from the address space rather than from the choice of client. Behind
carrier-grade NAT nobody outside can *initiate* a connection inwards. The only way two
hosts that both cannot be addressed ever meet is for both to dial **out** to some
mutually reachable third point, which then either introduces them — so they punch a
hole through both NATs and talk directly — or relays their traffic. That is how libp2p
works (circuit relay and AutoNAT), how WebRTC works (STUN and TURN), how Tailscale
works (DERP), and how Syncthing works. There is no arrangement of software that gets
two un-addressable hosts talking with no third party involved.

The good news is that the third party is tiny. Once the introduction is made, hole
punching succeeds through most carrier-grade NAT, and the media flows **directly**
between phone and laptop. The rendezvous carries a few kilobytes of signalling per
session.

Which is the actual argument for the decision below:

> **Tailscale is the networking half of that native app, already written.** It is a
> WireGuard data plane plus exactly that rendezvous, with hole punching, device
> identity and key rotation, shipping on all four target platforms.

Building the equivalent means ICE/STUN/TURN or libp2p in C++, a signalling server to
operate, a device pairing and identity system, then four client user interfaces, two
app-store review processes and a video player per platform. For one person that is
years of work, and it would *replace* a web interface that already exists and is
tested.

A native app still earns its place later, for the things a browser genuinely cannot
do: background photo upload from a phone, offline pinned files, hardware-decoded
playback, push notifications. Those are product features, not transport fixes, and
when they arrive they will talk to this same HTTP server over this same link.

---

## 3. The decision

**Tailscale, with port forwarding plus DDNS retained for anyone with a public IP.**

| | Who can view | Inbound port | Third party sees your files | Code changes |
|---|---|---|---|---|
| **Tailscale** | devices signed into your tailnet | none | no | none |
| Raw WireGuard | devices with a config file | yes, or a rented host | no | none |
| Port forward + DDNS | anyone with the link | yes | no | none |
| Cloudflare Tunnel | anyone with the link | none | **yes, in plaintext** | none |
| libp2p | nobody, without a Service Worker shim | none | no | very large |

Notes on the ones not chosen:

- **Raw WireGuard** does not actually solve this on its own. It still needs one
  endpoint with a reachable port, so it means either a forwarded port — back to the
  start — or renting a host to act as the hub. Tailscale is WireGuard plus the missing
  rendezvous.
- **Cloudflare Tunnel** does work through carrier-grade NAT, needs no client install,
  and gives a real certificate. It is rejected because it terminates TLS at
  Cloudflare's edge, so a third party reads your family's photos in the clear, which
  contradicts the point of self-hosting. Their terms also restrict serving large
  volumes of non-HTML content, which is to say video.
- **Tailscale Funnel** exposes a tailnet service publicly with no client install, and
  is useful for handing out an occasional link. It is bandwidth-limited and explicitly
  not intended for video, so it is not the default.

The honest cost of the choice: **every device that views the library has to have
Tailscale installed and signed in.** There is no plain shareable link for somebody who
has not done that. If that matters, the answer is a public IP (below), not a different
overlay.

---

## 4. Setting it up

### On the machine running NestDrive

1. Install Tailscale and sign in: `tailscale up`.
2. Enable HTTPS certificates for your tailnet, once, at
   <https://login.tailscale.com/admin/dns> → **HTTPS Certificates** → Enable. Without
   this, certificate requests fail and NestDrive reports the reason in the **Remote
   access** dialog.
3. In `config.ini`, set `useHttps=true` under `[Server]`. Leave `[Remote]` at its
   defaults.
4. Start NestDrive. It finds Tailscale, reads this machine's MagicDNS name, requests a
   certificate, and writes it to `certPath` and `keyPath`.

Step 3 is what makes NestDrive request a certificate at all. With `useHttps=false` it
asks for nothing and leaves `certPath`/`keyPath` alone — the library is still reachable
over the overlay, just as `http://`. That is not the hole it appears to be: **Tailscale
already encrypts the traffic end to end**, so what plain HTTP costs here is the
browser padlock and the secure-context features that depend on it, not
confidentiality.

The window's status bar then shows both addresses, and **Remote access…** shows the
remote one with its expiry.

### On each viewing device

Install Tailscale, sign into the same account, and open the URL from the **Remote
access** dialog — something like:

```
https://nestdrive.tail1a2b3c.ts.net:9090
```

**Use the name, not the `100.x.y.z` address.** The certificate is issued for the name,
so connecting by address produces exactly the browser warning that having a real
certificate is meant to remove.

### Renewal

Certificates last 90 days. NestDrive checks once at startup and then daily, renews
inside 14 days of expiry, and installs the result into the running server without a
restart — a download in progress is not interrupted, and connections opened afterwards
get the new certificate.

Nothing renews while the application is closed, which is why the check also runs on
startup. A machine switched off for two months renews on its next launch, before
anyone connects.

Adjust the window with `certRenewDays` in `[Remote]`. Zero or negative values are
ignored rather than obeyed; taken literally they would mean "renew only once it has
already expired".

---

## 5. The public link, on a connection that has a public IP

This is the path that gives you something to send to a relative who has installed
nothing. It needs three things: a public address, a forwarded port, and a certificate
for your DDNS hostname.

### Step 1 — confirm the connection

Open **Remote access…**. It must say **Public IP address**. If it says anything else,
stop — the rest cannot work, and the dialog explains why.

If your provider does not give you a public address, one is commonly available in
India for roughly ₹200–600 per month. Worth one phone call; it is the only way to get
a link that needs nothing installed.

### Step 2 — forward the port

Press **Forward the port** in the Remote access dialog. NestDrive asks the router over
UPnP, and confirms first — this adds a rule to equipment you own, which is why it is
never automatic and why `upnpPortForward` defaults to off.

If the router refuses or has UPnP disabled, add the rule by hand following
[importantsteps.md](../src/core/DdnsUpdater/importantsteps.md).

Common UPnP refusals, reported verbatim because their wording is the useful part:

- **error 718: ConflictInMappingEntry** — a rule for that port already exists,
  possibly pointing somewhere else. Remove it in the router first.
- **error 606** — the router will not accept mappings from this machine. Usually a
  setting; sometimes the provider has locked it.

### Step 3 — get a certificate for the hostname

**This certificate is yours, not NestDrive's.** On this path NestDrive only reads
`certPath` / `keyPath` and warns when they are wrong or expiring — it will never
overwrite them. That is deliberate: the tool below installs its own renewal task,
which keeps working while NestDrive is closed, and nothing in this process can match
that.

On Windows, [win-acme](https://www.win-acme.com/) is a single executable:

```
wacs.exe --target manual --host nestdrive.ddns.net --store pemfiles --pemfilespath .
```

Point `certPath` and `keyPath` at the files it writes. The loader accepts EC or RSA
keys and full chains, so whatever it produces will work.

**HTTP-01 needs inbound port 80 forwarded too**, for the challenge. If your provider
blocks inbound 80 — plausible on a residential line — use **DNS-01** instead, which
needs a DNS provider with an API. No-IP's free tier is not usable for that; DuckDNS
is, and so is a cheap domain on Cloudflare.

### Step 4 — test it the only way that counts

From a phone on **mobile data with Wi-Fi off and Tailscale switched off entirely**,
open `https://nestdrive.ddns.net:9090`. Padlock, no warning. Nothing short of that
proves it.

---

## 6. What is still unverified

Honest status, because none of the automated tests touch a real network — they run
against a fake CLI, so they prove the plumbing and nothing about the transport.

- Whether hole punching beats this particular provider's NAT, or whether traffic falls
  back to a relay. `tailscale status` says which; a relayed connection is noticeably
  slower for video. **Unmeasured.**
- Real-world throughput for video over the overlay. **Unmeasured.**
- Which TLS backend Qt selects in a packaged build. The test suite reports OpenSSL
  3.6.2 under its own environment, but Qt chooses at runtime based on whether it can
  load OpenSSL, and `windeployqt` does not ship those DLLs — so a packaged build
  likely falls back to Schannel. `core::describeTlsBackend()` logs the answer at
  startup; read it rather than assuming.
- The Schannel path specifically has never served a real request. Up to this work the
  only certificate it had ever been given was a single self-signed RSA file.
- **UPnP has never spoken to a real router.** The parsers are tested exhaustively
  against captured document shapes, but discovery, the SOAP calls and
  `AddPortMapping` have only ever run against fixtures. Expect the first real
  attempt to turn something up.
- Whether BSNL allows inbound port 80, which decides whether win-acme can use the
  HTTP-01 challenge or needs DNS-01. **Unmeasured.**

## 7. One bug worth remembering

The first version of the Tailscale path shipped with a defect that a real iPhone
found in about thirty seconds, and the shape of it is worth recording.

`CertRenewer` asked only *"is the certificate close to expiry?"*. The self-signed
certificate from `scripts/gen-cert.sh` is valid for years, so the answer was "nothing
to do" — and the real certificate was never requested at all. The server went on
presenting a certificate for `nestdrive.local` while being reached at a `.ts.net`
name, and Safari refused it, correctly.

Two things made it survive testing. The check conflated *"has it expired"* with *"is
it the right certificate"*, which are different questions. And the no-op path logged
nothing, so there was no trace of the decision anywhere — the only symptom was a
warning on a phone.

The renewer now tests the name as well as the date, via
`CertRenewer::covers()`, matching against subject alternative names only because
that is what browsers do. And **every** renewal decision is logged, including the ones
where nothing happens.
