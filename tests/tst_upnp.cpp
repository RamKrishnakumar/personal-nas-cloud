#include "core/Net/Upnp.h"

#include <QtTest>

/**
 * @file
 * @brief Coverage for the UPnP parsers.
 *
 * Follows http::RangeParser and http::RequestParser: the awkward real-world variation
 * is isolated into pure functions and tested from byte arrays, leaving the socket
 * layer thin.
 *
 * The variation is the point. SSDP only looks like HTTP and does not agree on header
 * case. Control URLs are usually relative, and sometimes relative to a `<URLBase>`
 * the document declares rather than to where the document was fetched from. The
 * service is buried two device levels down and routers advertise several. Each of
 * those is a way for this to work against one router brand and fail against the next,
 * which is exactly the sort of bug that is impossible to diagnose remotely.
 */
// The class is declared here, above the sample documents, and has to stay there.
//
// moc does not implement raw string literals. It reads the quotes inside one as
// ordinary string delimiters, and once out of step a SOAP namespace below — which
// contains a double slash, being a URL — reads as a line comment and swallows the
// rest of its line. moc then never reaches Q_OBJECT, writes an empty .moc file, and
// the only symptom is "undefined reference to vtable for TestUpnp" at link time,
// which says nothing whatever about string literals.
//
// Wrapping the samples in #ifndef Q_MOC_RUN does not help: moc still has to tokenise
// the skipped block to find the matching #endif, and loses that too. Declaring the
// class first is what works, because by the time moc desynchronises it already has
// everything it needs.
class TestUpnp : public QObject
{
    Q_OBJECT

private slots:
    // ── SSDP ──────────────────────────────────────────────────────────────
    void readsTheLocationHeader();
    void readsTheLocationHeaderWhateverItsCase();
    void ignoresARepyWithNoLocation();
    void ignoresAMalformedLocation();

    // ── Device description ────────────────────────────────────────────────
    void resolvesARelativeControlUrl();
    void prefersIpConnectionOverPpp();
    void acceptsPppWhenItIsAllThereIs();
    void honoursADeclaredUrlBase();
    void acceptsAnAbsoluteControlUrl();
    void reportsNothingWhenThereIsNoWanService();
    void reportsNothingForMalformedXml();

    // ── SOAP ──────────────────────────────────────────────────────────────
    void readsAResponseValue();
    void ignoresTheNamespacePrefix();
    void reportsNothingForAnAbsentElement();
    void readsAFaultWithItsCode();
    void findsNoFaultInASuccessfulResponse();
};

namespace {

/// A reply shaped like the ones real gateways send, with upper-case headers.
constexpr const char* SsdpReply =
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "LOCATION: http://192.168.0.1:5000/rootDesc.xml\r\n"
    "SERVER: Linux/3.4 UPnP/1.1 MiniUPnPd/2.1\r\n"
    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
    "\r\n";

/// The nesting a real InternetGatewayDevice uses, with a relative control URL.
constexpr const char* DeviceDescription = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <friendlyName>Test Router</friendlyName>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
        <controlURL>/ctl/L3F</controlURL>
      </service>
    </serviceList>
    <deviceList>
      <device>
        <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
        <deviceList>
          <device>
            <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
            <serviceList>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>
                <controlURL>/ctl/PPPConn</controlURL>
              </service>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
                <controlURL>/ctl/IPConn</controlURL>
              </service>
            </serviceList>
          </device>
        </deviceList>
      </device>
    </deviceList>
  </device>
</root>
)";

/// PPP only, which is what a DSL router offers — and what this household's is.
constexpr const char* PppOnlyDescription = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <device>
    <deviceList><device><deviceList><device>
      <serviceList>
        <service>
          <serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>
          <controlURL>/upnp/control/WANPPPConn1</controlURL>
        </service>
      </serviceList>
    </device></deviceList></device></deviceList>
  </device>
</root>
)";

constexpr const char* SuccessResponse = R"(<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">
  <s:Body>
    <u:GetExternalIPAddressResponse
        xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">
      <NewExternalIPAddress>111.91.226.113</NewExternalIPAddress>
    </u:GetExternalIPAddressResponse>
  </s:Body>
</s:Envelope>
)";

/// Fault 718 — the mapping already exists. One of the two common refusals.
constexpr const char* FaultResponse = R"(<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">
  <s:Body>
    <s:Fault>
      <faultcode>s:Client</faultcode>
      <faultstring>UPnPError</faultstring>
      <detail>
        <UPnPError xmlns="urn:schemas-upnp-org:control-1-0">
          <errorCode>718</errorCode>
          <errorDescription>ConflictInMappingEntry</errorDescription>
        </UPnPError>
      </detail>
    </s:Fault>
  </s:Body>
</s:Envelope>
)";

} // namespace

// ── SSDP ────────────────────────────────────────────────────────────────────

void TestUpnp::readsTheLocationHeader()
{
    const QUrl location = core::upnp::parseSsdpLocation(QByteArray(SsdpReply));

    QCOMPARE(location.toString(),
             QStringLiteral("http://192.168.0.1:5000/rootDesc.xml"));
}

void TestUpnp::readsTheLocationHeaderWhateverItsCase()
{
    // SSDP resembles HTTP but is not HTTP, and routers in the wild send all three of
    // these. A case-sensitive match finds nothing on whichever ones disagree with
    // the guess, which presents as "no UPnP on this network".
    for (const char* header : { "LOCATION", "Location", "location" }) {
        const QByteArray reply = QByteArray("HTTP/1.1 200 OK\r\n")
                                 + header + ": http://10.0.0.1/desc.xml\r\n\r\n";

        QVERIFY2(core::upnp::parseSsdpLocation(reply).isValid(), header);
    }
}

void TestUpnp::ignoresARepyWithNoLocation()
{
    const QByteArray reply = "HTTP/1.1 200 OK\r\nST: something\r\n\r\n";
    QVERIFY(!core::upnp::parseSsdpLocation(reply).isValid());
}

void TestUpnp::ignoresAMalformedLocation()
{
    // A header present but carrying nothing usable. Returning an invalid URL means
    // discovery keeps listening for a better answer instead of trying to fetch this.
    const QByteArray reply = "HTTP/1.1 200 OK\r\nLOCATION: \r\n\r\n";
    QVERIFY(!core::upnp::parseSsdpLocation(reply).isValid());
}

// ── Device description ──────────────────────────────────────────────────────

void TestUpnp::resolvesARelativeControlUrl()
{
    const core::UpnpService service = core::upnp::parseDeviceDescription(
        QByteArray(DeviceDescription),
        QUrl(QStringLiteral("http://192.168.0.1:5000/rootDesc.xml")));

    QVERIFY(service.isValid());

    // Against the document's URL, not its root. Getting this wrong produces a
    // plausible-looking URL that 404s, and the symptom is a router that "does not
    // support UPnP".
    QCOMPARE(service.controlUrl.toString(),
             QStringLiteral("http://192.168.0.1:5000/ctl/IPConn"));
}

void TestUpnp::prefersIpConnectionOverPpp()
{
    // The document lists PPP first. Preference order has to win over document order,
    // or which service gets used depends on how the router happened to lay out its
    // XML.
    const core::UpnpService service = core::upnp::parseDeviceDescription(
        QByteArray(DeviceDescription),
        QUrl(QStringLiteral("http://192.168.0.1:5000/rootDesc.xml")));

    QCOMPARE(service.serviceType,
             QStringLiteral("urn:schemas-upnp-org:service:WANIPConnection:1"));
}

void TestUpnp::acceptsPppWhenItIsAllThereIs()
{
    // Not a fallback worth skipping: PPPoE is what this household's connection runs,
    // so a client that only understood WANIPConnection would find nothing on the
    // very router this was built for.
    const core::UpnpService service = core::upnp::parseDeviceDescription(
        QByteArray(PppOnlyDescription),
        QUrl(QStringLiteral("http://192.168.1.1:49152/desc.xml")));

    QVERIFY(service.isValid());
    QCOMPARE(service.serviceType,
             QStringLiteral("urn:schemas-upnp-org:service:WANPPPConnection:1"));
    QCOMPARE(service.controlUrl.toString(),
             QStringLiteral("http://192.168.1.1:49152/upnp/control/WANPPPConn1"));
}

void TestUpnp::honoursADeclaredUrlBase()
{
    // Some routers serve the description from one port and accept control requests
    // on another, and say so with URLBase. Ignoring it sends every SOAP call to the
    // wrong port.
    QByteArray xml = QByteArray(DeviceDescription);

    // Inserted as a child of <root>, immediately before the first <device>, which is
    // where a real document carries it.
    const int firstDevice = xml.indexOf("<device>");
    QVERIFY(firstDevice > 0);
    xml.insert(firstDevice, "<URLBase>http://192.168.0.1:49000/</URLBase>");

    const core::UpnpService service = core::upnp::parseDeviceDescription(
        xml, QUrl(QStringLiteral("http://192.168.0.1:5000/rootDesc.xml")));

    QVERIFY(service.isValid());
    QCOMPARE(service.controlUrl.toString(),
             QStringLiteral("http://192.168.0.1:49000/ctl/IPConn"));
}

void TestUpnp::acceptsAnAbsoluteControlUrl()
{
    QByteArray xml = QByteArray(DeviceDescription);
    xml.replace("<controlURL>/ctl/IPConn</controlURL>",
                "<controlURL>http://10.1.1.1:80/other</controlURL>");

    const core::UpnpService service = core::upnp::parseDeviceDescription(
        xml, QUrl(QStringLiteral("http://192.168.0.1:5000/rootDesc.xml")));

    // resolved() must leave an already-absolute URL alone.
    QCOMPARE(service.controlUrl.toString(), QStringLiteral("http://10.1.1.1:80/other"));
}

void TestUpnp::reportsNothingWhenThereIsNoWanService()
{
    // A UPnP device that is not a gateway — a printer, a television. Both answer
    // SSDP searches on a home network.
    const QByteArray xml = R"(<?xml version="1.0"?>
<root><device>
  <serviceList>
    <service>
      <serviceType>urn:schemas-upnp-org:service:Printer:1</serviceType>
      <controlURL>/ctl/print</controlURL>
    </service>
  </serviceList>
</device></root>)";

    QVERIFY(!core::upnp::parseDeviceDescription(
                 xml, QUrl(QStringLiteral("http://192.168.0.5/d.xml")))
                 .isValid());
}

void TestUpnp::reportsNothingForMalformedXml()
{
    const QByteArray truncated = QByteArray(DeviceDescription).left(220);

    QVERIFY(!core::upnp::parseDeviceDescription(
                 truncated, QUrl(QStringLiteral("http://192.168.0.1/d.xml")))
                 .isValid());

    QVERIFY(!core::upnp::parseDeviceDescription(
                 QByteArray("not xml at all"),
                 QUrl(QStringLiteral("http://192.168.0.1/d.xml")))
                 .isValid());
}

// ── SOAP ────────────────────────────────────────────────────────────────────

void TestUpnp::readsAResponseValue()
{
    QCOMPARE(core::upnp::parseSoapValue(QByteArray(SuccessResponse),
                                        QStringLiteral("NewExternalIPAddress")),
             QStringLiteral("111.91.226.113"));
}

void TestUpnp::ignoresTheNamespacePrefix()
{
    // The prefix a router picks for the response body varies; `u:` is conventional
    // but not required. Matching on the qualified name would work until one of them
    // chose differently.
    QByteArray xml = QByteArray(SuccessResponse);
    xml.replace("u:", "ns1:").replace("xmlns:u=", "xmlns:ns1=");

    QCOMPARE(core::upnp::parseSoapValue(xml,
                                        QStringLiteral("NewExternalIPAddress")),
             QStringLiteral("111.91.226.113"));
}

void TestUpnp::reportsNothingForAnAbsentElement()
{
    QVERIFY(core::upnp::parseSoapValue(QByteArray(SuccessResponse),
                                       QStringLiteral("NewLeaseDuration"))
                .isEmpty());
}

void TestUpnp::readsAFaultWithItsCode()
{
    const QString fault = core::upnp::parseSoapFault(QByteArray(FaultResponse));

    // Both halves. "ConflictInMappingEntry" alone is barely English, and 718 alone
    // is unreadable — but together they are something that can be acted on or looked
    // up. These arrive as HTTP 500, so without this the owner would be shown
    // "Internal Server Error" instead.
    QVERIFY2(fault.contains(QStringLiteral("718")), qPrintable(fault));
    QVERIFY2(fault.contains(QStringLiteral("ConflictInMappingEntry")),
             qPrintable(fault));
}

void TestUpnp::findsNoFaultInASuccessfulResponse()
{
    // The fault check runs before the transport error on every reply, so a false
    // positive here would turn every successful call into a failure.
    QVERIFY(core::upnp::parseSoapFault(QByteArray(SuccessResponse)).isEmpty());
    QVERIFY(core::upnp::parseSoapFault(QByteArray()).isEmpty());
}

QTEST_MAIN(TestUpnp)
#include "tst_upnp.moc"
