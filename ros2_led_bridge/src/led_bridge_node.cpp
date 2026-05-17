#include <rclcpp/rclcpp.hpp>
#include <ros2_led_bridge/msg/led_command.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#define SOCKET_PATH "/run/leds.sock"

using LedCommand = ros2_led_bridge::msg::LedCommand;

class LedBridgeNode : public rclcpp::Node
{
public:
    LedBridgeNode()
    : Node("led_bridge"), sock_fd_(-1)
    {
        connect_socket();

        sub_ = create_subscription<LedCommand>(
            "/led_strip", 10,
            std::bind(&LedBridgeNode::on_command, this, std::placeholders::_1));

        RCLCPP_INFO(get_logger(), "LED bridge ready — daemon %s",
                    sock_fd_ >= 0 ? "connected" : "not yet available");
    }

    ~LedBridgeNode()
    {
        if (sock_fd_ >= 0) close(sock_fd_);
    }

private:
    int sock_fd_;
    rclcpp::Subscription<LedCommand>::SharedPtr sub_;

    void connect_socket()
    {
        if (sock_fd_ >= 0) { close(sock_fd_); sock_fd_ = -1; }

        sock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd_ < 0) return;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (connect(sock_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
            close(sock_fd_);
            sock_fd_ = -1;
        }
    }

    void on_command(const LedCommand::SharedPtr msg)
    {
        if (sock_fd_ < 0) {
            connect_socket();
            if (sock_fd_ < 0) {
                RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                                     "LED daemon not reachable at " SOCKET_PATH);
                return;
            }
            RCLCPP_INFO(get_logger(), "Reconnected to LED daemon");
        }

        /* Protocol: [index, r, g, b, effect, period_ms_hi, period_ms_lo] */
        uint8_t cmd[7] = {
            msg->index,
            msg->r,
            msg->g,
            msg->b,
            msg->effect,
            static_cast<uint8_t>(msg->period_ms >> 8),
            static_cast<uint8_t>(msg->period_ms & 0xFF)
        };

        if (write(sock_fd_, cmd, sizeof(cmd)) < 0) {
            RCLCPP_WARN(get_logger(), "write failed (%s), reconnecting", strerror(errno));
            connect_socket();
        }
    }
};

int main(int argc, char *argv[])
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<LedBridgeNode>());
    rclcpp::shutdown();
    return 0;
}
