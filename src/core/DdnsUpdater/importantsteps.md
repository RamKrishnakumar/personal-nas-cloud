> ## ⚠️ Requires a real public IP — and NestDrive can now tell you
>
> Everything below assumes the address in your router's WAN status is the address the
> internet sees. On many Indian home connections it is not: the provider runs its own
> NAT in front of you, and a forwarding rule then cannot work at all, because nothing
> on the internet can address your router.
>
> **Open NestDrive's Remote access… dialog first.** It asks the router over UPnP,
> compares the answer with what the internet reports, and tells you which situation
> you are in. If it says *Behind the provider's NAT*, stop here — no setting on this
> page will help; use Tailscale instead. If it says *Public IP address*, everything
> below applies, and the dialog's **Forward the port** button can do step 1 for you.
>
> If it says *Not determined*, the router would not answer. Make the comparison by
> hand — WAN IP on the router's admin page against whatismyip.com — and record the
> result as `[Remote] connectionType` in `config.ini`.
>
> Both situations exist in this household: the Airtel line is behind the provider's
> NAT, the BSNL line is not. That is why NestDrive measures rather than assumes, and
> why this document is one of two paths rather than the only one.
>
> See [docs/REMOTE-ACCESS.md](../../../docs/REMOTE-ACCESS.md) — in particular §5, for
> the certificate step that gives the shareable link a padlock.

Internet → <WAN Ip address>:9090 → Router Port Forward → 192.168.1.100:9090 → NestDrive
              (your public IP)                            (your laptop)
Three parts:

Part	What	Where
1	Router port forwarding rule	Router admin panel (5 mins)
2	No-IP free subdomain	www.noip.org website (5 mins)
3	DdnsUpdater class in app	C++ code — auto-updates your IP
### Part 1 — Router Port Forwarding

Go to http://192.168.1.1 or http://192.168.0.1 → log in

Click Application tab (top menu)

Look for Port Forwarding or Virtual Server

Add a new rule:

| Field              | Value                                |
| ------------------ | ------------------------------------ |
| Name / Description | NestDrive                            |
| Server IP Address  | 192.168.1.100 ← your laptop's LAN IP |
| Internet IP        | get from whatismyip.com and paster   |
| Start Port         | 9090                                 |
| End Port           | 9090                                 |
| Port               | 9090                                 |
| Protocol           | TCP                                  |
| Status / Enable    | ✅ Enabled                          |
Save/Apply

After Saving the Rule
Test it immediately from your Mac using mobile data (turn off WiFi on your Mac, use hotspot or mobile data) then open:

http://<WAN-Ip-address get from whatismyip.com and match from your admin panel of wifi>:9090

If NestDrive loads → port forwarding is working perfectly.
If it times out → the router needs a reboot after saving the rule (some routers require this) 

Once confirmed working move to Part 2

### Part 2 — Register Free DuckDNS Subdomain
No-IP is a popular alternative that works fine in India:

Go to https://www.noip.com

Click Sign Up Free

Fill in:

Hostname: nestdrive (creates nestdrive.ddns.net)

Email + Password

Confirm email

Log in → Dynamic DNS → No-IP Hostnames

Your hostname nestdrive.ddns.net is created pointing to your current IP

Copy your username, password and hostname — needed for the code

### Part 3 — Code: Add DdnsUpdater Class
Once you complete Parts 1 and 2 and have your No-IP token ready, we'll add a new class that:

Pings DuckDNS API every 5 minutes to keep your IP updated

Shows the public URL http://hostname.ddns.org:9090 in the app UI alongside the LAN URL


## Implementation Plan
### What We'll Add

📁 MyNAS/
├── config.ini              ← Created by user locally, NEVER pushed to GitHub
├── config.ini.example      ← Template committed to GitHub (no real credentials)
├── src/core/
│   ├── AppConfig.h         ← New: reads config.ini into memory
│   └── AppConfig.cpp       ← New: parses the INI file

### Step 1 — Add to .gitignore
Open your .gitignore and add:

# Local config — contains credentials, never commit this
config.ini

### Step 2 — Create config.ini.example
This file is committed to GitHub — it's a safe template showing what fields are needed without real values:

### Step 3 — Create src/core/AppConfig.h

### Step 4 — Create src/core/AppConfig.cpp