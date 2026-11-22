#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace protocol{

    enum class MessageType {
        MENU_REQUEST,
        MENU_RESPONSE,
        ORDER_REQUEST,
        ORDER_ACCEPTED,
        ORDER_REFUSED,
        ORDER_READY,
        PROVIDER_IDENTIFY,
        DELIVERY,
        LOW_STOCK,
        HEARTBEAT,
        ERROR
    };

    enum class ErrorCode {
        INVALID_MESSAGE,
        MISSING_FIELD,
        INVALID_QUANTITY,
        UNKNOWN_PRODUCT,
        UNAUTHORIZED_OPERATION,
        INVALID_DELIVERY
    };

    enum class Product {
        coffee,
        tea,
        croissant
    };

    namespace detail {
        template <typename Enum>
        struct EnumEntry {
            Enum value;
            std::string_view name;
        };

        template <typename Enum>
        struct EnumTraits;

        template <>
        struct EnumTraits<MessageType> {
            static constexpr auto entries = std::to_array<EnumEntry<MessageType>>({
                {MessageType::MENU_REQUEST, "MENU_REQUEST"},
                {MessageType::MENU_RESPONSE, "MENU_RESPONSE"},
                {MessageType::ORDER_REQUEST, "ORDER_REQUEST"},
                {MessageType::ORDER_ACCEPTED, "ORDER_ACCEPTED"},
                {MessageType::ORDER_REFUSED, "ORDER_REFUSED"},
                {MessageType::ORDER_READY, "ORDER_READY"},
                {MessageType::PROVIDER_IDENTIFY, "PROVIDER_IDENTIFY"},
                {MessageType::DELIVERY, "DELIVERY"},
                {MessageType::LOW_STOCK, "LOW_STOCK"},
                {MessageType::HEARTBEAT, "HEARTBEAT"},
                {MessageType::ERROR, "ERROR"},
            });
        };

        template <>
        struct EnumTraits<ErrorCode> {
            static constexpr auto entries = std::to_array<EnumEntry<ErrorCode>>({
                {ErrorCode::INVALID_MESSAGE, "INVALID_MESSAGE"},
                {ErrorCode::MISSING_FIELD, "MISSING_FIELD"},
                {ErrorCode::INVALID_QUANTITY, "INVALID_QUANTITY"},
                {ErrorCode::UNKNOWN_PRODUCT, "UNKNOWN_PRODUCT"},
                {ErrorCode::UNAUTHORIZED_OPERATION, "UNAUTHORIZED_OPERATION"},
                {ErrorCode::INVALID_DELIVERY, "INVALID_DELIVERY"},
            });
        };

        template <>
        struct EnumTraits<Product> {
            static constexpr auto entries = std::to_array<EnumEntry<Product>>({
                {Product::coffee, "coffee"},
                {Product::tea, "tea"},
                {Product::croissant, "croissant"},
            });
        };
    }

    // Enum converters
    template <typename Enum>
    requires requires { detail::EnumTraits<Enum>::entries; }
    constexpr std::string_view to_string(Enum value) noexcept {
        for (const auto& entry : detail::EnumTraits<Enum>::entries) {
            if (entry.value == value) {
                return entry.name;
            }
        }
        return {};
    }

    template <typename Enum>
    requires requires { detail::EnumTraits<Enum>::entries; }
    constexpr std::optional<Enum> from_string(std::string_view sv) noexcept {
        for (const auto& entry : detail::EnumTraits<Enum>::entries) {
            if (entry.name == sv) {
                return entry.value;
            }
        }
        return std::nullopt;
    }

    // Shared data structure
    struct Items {
        int coffee{};
        int tea{};
        int croissant{};
    };

    // Reusable message structures
    struct EmptyMessage {
        MessageType type{};
    };

    struct ItemsMessage {
        MessageType type{};
        Items items{};
    };

    struct AcceptedMessage {
        MessageType type{};
        int orderId{};
        int preparationTime{};
    };

    struct RefusedMessage {
        MessageType type{};
        std::string reason{};
    };

    struct OrderIdMessage {
        MessageType type{};
        int orderId{};
    };

    struct LowStockMessage {
        MessageType type{};
        Product product{};
        int remaining{};
    };

    struct ErrorMessage {
        MessageType type{};
        ErrorCode code{};
        std::string message{};
    };

    // Protocol validation exception carrying the ErrorCode and descriptive message
    class ProtocolException : public std::runtime_error {
    public:
        ProtocolException(ErrorCode code, const std::string& message)
            : std::runtime_error(message), code_(code) {}

        ErrorCode code() const noexcept { return code_; }

    private:
        ErrorCode code_;
    };

    // JSON helper serialisers / deserialisers
    using json = nlohmann::json;

    namespace detail {
        template <typename Enum>
        inline void enum_to_json(json& j, Enum value, ErrorCode err_code, std::string_view label) {
            const auto sv = to_string(value);
            if (sv.empty()) {
                throw ProtocolException(err_code, "Invalid " + std::string(label) + " value");
            }
            j = sv;
        }

        template <typename Enum>
        inline void enum_from_json(const json& j,
                                   Enum& value,
                                   ErrorCode type_err,
                                   ErrorCode value_err,
                                   std::string_view label) {
            if (!j.is_string()) {
                throw ProtocolException(type_err, std::string(label) + " must be a string");
            }
            const auto& str = j.get_ref<const std::string&>();
            const auto parsed = from_string<Enum>(str);
            if (!parsed) {
                throw ProtocolException(value_err, "Unknown " + std::string(label) + ": " + str);
            }
            value = *parsed;
        }

        inline void validate_object_keys(const json& j,
                                         std::initializer_list<std::string_view> required,
                                         std::initializer_list<std::string_view> allowed) {
            if (!j.is_object()) {
                throw ProtocolException(ErrorCode::INVALID_MESSAGE, "Message must be a JSON object");
            }
            for (const auto req : required) {
                if (!j.contains(req)) {
                    throw ProtocolException(ErrorCode::MISSING_FIELD,
                        "Missing required field: " + std::string(req));
                }
            }
            for (const auto& item : j.items()) {
                bool is_allowed = false;
                for (const auto ok : allowed) {
                    if (item.key() == ok) {
                        is_allowed = true;
                        break;
                    }
                }
                if (!is_allowed) {
                    throw ProtocolException(ErrorCode::INVALID_MESSAGE,
                        "Unknown field: " + item.key());
                }
            }
        }

        inline int parse_int(const json& j,
                             std::string_view field_name,
                             int min_val,
                             int max_val,
                             ErrorCode err_code) {
            if (j.is_number_unsigned()) {
                const auto val = j.get<std::uint64_t>();
                if ((min_val > 0 && val < static_cast<std::uint64_t>(min_val)) ||
                    max_val < 0 || val > static_cast<std::uint64_t>(max_val)) {
                    throw ProtocolException(err_code,
                        "Field '" + std::string(field_name) + "' must be an integer between " +
                        std::to_string(min_val) + " and " + std::to_string(max_val));
                }
                return static_cast<int>(val);
            }
            if (j.is_number_integer()) {
                const auto val = j.get<std::int64_t>();
                if (val < min_val || val > max_val) {
                    throw ProtocolException(err_code,
                        "Field '" + std::string(field_name) + "' must be an integer between " +
                        std::to_string(min_val) + " and " + std::to_string(max_val));
                }
                return static_cast<int>(val);
            }
            throw ProtocolException(err_code,
                "Field '" + std::string(field_name) + "' must be an integer");
        }

        inline std::string parse_string_field(const json& j, std::string_view field_name) {
            const auto& val = j.at(field_name);
            if (!val.is_string()) {
                throw ProtocolException(ErrorCode::INVALID_MESSAGE,
                    "Field '" + std::string(field_name) + "' must be a string");
            }
            return val.get<std::string>();
        }

        inline void validate_message_type(MessageType actual,
                                          std::initializer_list<MessageType> expected,
                                          std::string_view struct_name) {
            for (const auto candidate : expected) {
                if (actual == candidate) {
                    return;
                }
            }
            const auto name = to_string(actual);
            throw ProtocolException(ErrorCode::INVALID_MESSAGE,
                "Invalid message type '" +
                std::string(name.empty() ? "<unknown>" : name) +
                "' for " + std::string(struct_name));
        }

        inline bool has_positive_item(const Items& items) noexcept {
            return items.coffee > 0 || items.tea > 0 || items.croissant > 0;
        }
    }

    // Enum JSON converters
    inline void to_json(json& j, MessageType type) {
        detail::enum_to_json(j, type, ErrorCode::INVALID_MESSAGE, "MessageType");
    }

    inline void from_json(const json& j, MessageType& type) {
        detail::enum_from_json(j, type, ErrorCode::INVALID_MESSAGE, ErrorCode::INVALID_MESSAGE, "MessageType");
    }

    inline void to_json(json& j, ErrorCode code) {
        detail::enum_to_json(j, code, ErrorCode::INVALID_MESSAGE, "ErrorCode");
    }

    inline void from_json(const json& j, ErrorCode& code) {
        detail::enum_from_json(j, code, ErrorCode::INVALID_MESSAGE, ErrorCode::INVALID_MESSAGE, "ErrorCode");
    }

    inline void to_json(json& j, Product product) {
        detail::enum_to_json(j, product, ErrorCode::UNKNOWN_PRODUCT, "Product");
    }

    inline void from_json(const json& j, Product& product) {
        detail::enum_from_json(j, product, ErrorCode::INVALID_MESSAGE, ErrorCode::UNKNOWN_PRODUCT, "Product");
    }

    // Items
    inline void to_json(json& j, const Items& items) {
        if (items.coffee < 0 || items.tea < 0 || items.croissant < 0) {
            throw ProtocolException(ErrorCode::INVALID_QUANTITY, "Item quantities cannot be negative");
        }
        j = json{
            {"coffee", items.coffee},
            {"tea", items.tea},
            {"croissant", items.croissant}
        };
    }

    inline void from_json(const json& j, Items& items) {
        if (!j.is_object()) {
            throw ProtocolException(ErrorCode::INVALID_MESSAGE, "Field 'items' must be a JSON object");
        }
        Items parsed{};
        constexpr int max_qty = std::numeric_limits<int>::max();
        for (const auto& entry : j.items()) {
            const auto& key = entry.key();
            if (key == "coffee") {
                parsed.coffee = detail::parse_int(entry.value(), "coffee", 0, max_qty, ErrorCode::INVALID_QUANTITY);
            } else if (key == "tea") {
                parsed.tea = detail::parse_int(entry.value(), "tea", 0, max_qty, ErrorCode::INVALID_QUANTITY);
            } else if (key == "croissant") {
                parsed.croissant = detail::parse_int(entry.value(), "croissant", 0, max_qty, ErrorCode::INVALID_QUANTITY);
            } else {
                throw ProtocolException(ErrorCode::UNKNOWN_PRODUCT, "Unknown product: " + key);
            }
        }
        items = parsed;
    }

    // EmptyMessage (MENU_REQUEST, PROVIDER_IDENTIFY, HEARTBEAT)
    inline void to_json(json& j, const EmptyMessage& msg) {
        detail::validate_message_type(msg.type,
            {MessageType::MENU_REQUEST, MessageType::PROVIDER_IDENTIFY, MessageType::HEARTBEAT}, "EmptyMessage");
        j = json{
            {"type", msg.type}
        };
    }

    inline void from_json(const json& j, EmptyMessage& msg) {
        detail::validate_object_keys(j, {"type"}, {"type"});
        EmptyMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type,
            {MessageType::MENU_REQUEST, MessageType::PROVIDER_IDENTIFY, MessageType::HEARTBEAT}, "EmptyMessage");
        msg = parsed;
    }

    // ItemsMessage (ORDER_REQUEST, MENU_RESPONSE, DELIVERY)
    inline void to_json(json& j, const ItemsMessage& msg) {
        detail::validate_message_type(msg.type,
            {MessageType::ORDER_REQUEST, MessageType::MENU_RESPONSE, MessageType::DELIVERY}, "ItemsMessage");
        json items_json = msg.items;
        if (msg.type == MessageType::ORDER_REQUEST && !detail::has_positive_item(msg.items)) {
            throw ProtocolException(ErrorCode::INVALID_QUANTITY,
                "Order must contain at least one item with positive quantity");
        }
        if (msg.type == MessageType::DELIVERY && !detail::has_positive_item(msg.items)) {
            throw ProtocolException(ErrorCode::INVALID_DELIVERY,
                "Delivery must contain at least one item with positive quantity");
        }
        j = json{
            {"type", msg.type},
            {"items", std::move(items_json)}
        };
    }

    inline void from_json(const json& j, ItemsMessage& msg) {
        detail::validate_object_keys(j, {"type", "items"}, {"type", "items"});
        ItemsMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type,
            {MessageType::ORDER_REQUEST, MessageType::MENU_RESPONSE, MessageType::DELIVERY}, "ItemsMessage");
        const auto& items_json = j.at("items");
        items_json.get_to(parsed.items);

        if (parsed.type == MessageType::MENU_RESPONSE) {
            for (const auto field : {"coffee", "tea", "croissant"}) {
                if (!items_json.contains(field)) {
                    throw ProtocolException(ErrorCode::MISSING_FIELD,
                        std::string("Stock report missing required product field: ") + field);
                }
            }
        } else if (parsed.type == MessageType::ORDER_REQUEST && !detail::has_positive_item(parsed.items)) {
            throw ProtocolException(ErrorCode::INVALID_QUANTITY,
                "Order must contain at least one item with positive quantity");
        } else if (parsed.type == MessageType::DELIVERY && !detail::has_positive_item(parsed.items)) {
            throw ProtocolException(ErrorCode::INVALID_DELIVERY,
                "Delivery must contain at least one item with positive quantity");
        }
        msg = parsed;
    }

    // AcceptedMessage (ORDER_ACCEPTED)
    inline void to_json(json& j, const AcceptedMessage& msg) {
        detail::validate_message_type(msg.type, {MessageType::ORDER_ACCEPTED}, "AcceptedMessage");
        if (msg.orderId <= 0) {
            throw ProtocolException(ErrorCode::INVALID_MESSAGE, "orderId must be positive");
        }
        if (msg.preparationTime < 0) {
            throw ProtocolException(ErrorCode::INVALID_MESSAGE, "preparationTime cannot be negative");
        }
        j = json{
            {"type", msg.type},
            {"orderId", msg.orderId},
            {"preparationTime", msg.preparationTime}
        };
    }

    inline void from_json(const json& j, AcceptedMessage& msg) {
        detail::validate_object_keys(j, {"type", "orderId", "preparationTime"},
            {"type", "orderId", "preparationTime"});
        AcceptedMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type, {MessageType::ORDER_ACCEPTED}, "AcceptedMessage");
        constexpr int max_int = std::numeric_limits<int>::max();
        parsed.orderId = detail::parse_int(j.at("orderId"), "orderId", 1, max_int, ErrorCode::INVALID_MESSAGE);
        parsed.preparationTime = detail::parse_int(j.at("preparationTime"), "preparationTime", 0, max_int, ErrorCode::INVALID_MESSAGE);
        msg = parsed;
    }

    // RefusedMessage (ORDER_REFUSED)
    inline void to_json(json& j, const RefusedMessage& msg) {
        detail::validate_message_type(msg.type, {MessageType::ORDER_REFUSED}, "RefusedMessage");
        j = json{
            {"type", msg.type},
            {"reason", msg.reason}
        };
    }

    inline void from_json(const json& j, RefusedMessage& msg) {
        detail::validate_object_keys(j, {"type", "reason"}, {"type", "reason"});
        RefusedMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type, {MessageType::ORDER_REFUSED}, "RefusedMessage");
        parsed.reason = detail::parse_string_field(j, "reason");
        msg = std::move(parsed);
    }

    // OrderIdMessage (ORDER_READY)
    inline void to_json(json& j, const OrderIdMessage& msg) {
        detail::validate_message_type(msg.type, {MessageType::ORDER_READY}, "OrderIdMessage");
        if (msg.orderId <= 0) {
            throw ProtocolException(ErrorCode::INVALID_MESSAGE, "orderId must be positive");
        }
        j = json{
            {"type", msg.type},
            {"orderId", msg.orderId}
        };
    }

    inline void from_json(const json& j, OrderIdMessage& msg) {
        detail::validate_object_keys(j, {"type", "orderId"}, {"type", "orderId"});
        OrderIdMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type, {MessageType::ORDER_READY}, "OrderIdMessage");
        parsed.orderId = detail::parse_int(j.at("orderId"), "orderId", 1, std::numeric_limits<int>::max(), ErrorCode::INVALID_MESSAGE);
        msg = parsed;
    }

    // LowStockMessage (LOW_STOCK)
    inline void to_json(json& j, const LowStockMessage& msg) {
        detail::validate_message_type(msg.type, {MessageType::LOW_STOCK}, "LowStockMessage");
        if (msg.remaining < 0 || msg.remaining > 4) {
            throw ProtocolException(ErrorCode::INVALID_QUANTITY, "remaining must be between 0 and 4");
        }
        j = json{
            {"type", msg.type},
            {"product", msg.product},
            {"remaining", msg.remaining}
        };
    }

    inline void from_json(const json& j, LowStockMessage& msg) {
        detail::validate_object_keys(j, {"type", "product", "remaining"}, {"type", "product", "remaining"});
        LowStockMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type, {MessageType::LOW_STOCK}, "LowStockMessage");
        j.at("product").get_to(parsed.product);
        parsed.remaining = detail::parse_int(j.at("remaining"), "remaining", 0, 4, ErrorCode::INVALID_QUANTITY);
        msg = parsed;
    }

    // ErrorMessage (ERROR)
    inline void to_json(json& j, const ErrorMessage& msg) {
        detail::validate_message_type(msg.type, {MessageType::ERROR}, "ErrorMessage");
        j = json{
            {"type", msg.type},
            {"code", msg.code},
            {"message", msg.message}
        };
    }

    inline void from_json(const json& j, ErrorMessage& msg) {
        detail::validate_object_keys(j, {"type", "code", "message"}, {"type", "code", "message"});
        ErrorMessage parsed{};
        j.at("type").get_to(parsed.type);
        detail::validate_message_type(parsed.type, {MessageType::ERROR}, "ErrorMessage");
        j.at("code").get_to(parsed.code);
        parsed.message = detail::parse_string_field(j, "message");
        msg = std::move(parsed);
    }

    // Top-level message type inspection & variant dispatch
    using AnyMessage = std::variant<
        EmptyMessage,
        ItemsMessage,
        AcceptedMessage,
        RefusedMessage,
        OrderIdMessage,
        LowStockMessage,
        ErrorMessage
    >;

    inline MessageType parse_message_type(const json& j) {
        if (!j.is_object()) {
            throw ProtocolException(ErrorCode::INVALID_MESSAGE, "Message must be a JSON object");
        }
        if (!j.contains("type")) {
            throw ProtocolException(ErrorCode::MISSING_FIELD, "Missing required field: type");
        }
        return j.at("type").get<MessageType>();
    }

    inline AnyMessage deserialize_message(const json& j) {
        switch (parse_message_type(j)) {
            case MessageType::MENU_REQUEST:
            case MessageType::PROVIDER_IDENTIFY:
            case MessageType::HEARTBEAT:
                return j.get<EmptyMessage>();
            case MessageType::ORDER_REQUEST:
            case MessageType::MENU_RESPONSE:
            case MessageType::DELIVERY:
                return j.get<ItemsMessage>();
            case MessageType::ORDER_ACCEPTED:
                return j.get<AcceptedMessage>();
            case MessageType::ORDER_REFUSED:
                return j.get<RefusedMessage>();
            case MessageType::ORDER_READY:
                return j.get<OrderIdMessage>();
            case MessageType::LOW_STOCK:
                return j.get<LowStockMessage>();
            case MessageType::ERROR:
                return j.get<ErrorMessage>();
        }
        throw ProtocolException(ErrorCode::INVALID_MESSAGE, "Unknown message type");
    }

}


#endif
