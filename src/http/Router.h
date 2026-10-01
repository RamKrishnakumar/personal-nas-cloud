#pragma once

namespace http {

class Connection;
struct Request;

/**
 * @brief Handles one parsed request.
 *
 * Separating this from Connection keeps transport concerns (framing, TLS,
 * keep-alive, streaming) apart from policy (what a path means, who may read it).
 * Step 13 inserts the authentication gate by wrapping a Router rather than by
 * editing every handler.
 *
 * @warning Implementations must be thread-safe. One instance is shared by every
 *          worker thread, and two requests can be in handle() at the same
 *          moment. Keep handlers stateless, or guard whatever state they need.
 */
class Router
{
public:
    virtual ~Router() = default;

    /**
     * @brief Produce a response for @p request on @p connection.
     *
     * The implementation must call exactly one of Connection's send functions.
     * Returning without sending leaves the client waiting until the idle timeout
     * closes the socket.
     */
    virtual void handle(Connection& connection, const Request& request) = 0;
};

} // namespace http
