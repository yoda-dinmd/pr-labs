import com.fasterxml.jackson.annotation.JsonInclude;

@JsonInclude(JsonInclude.Include.NON_NULL)
public class Protocol {

    public enum MessageType {
        MENU_REQUEST,
        MENU_RESPONSE,
        ORDER_REQUEST,
        ORDER_ACCEPTED,
        ORDER_REFUSED,
        ORDER_READY,
        PROVIDER_IDENTIFY,
        DELIVERY,
        LOW_STOCK,
        ERROR
    }

    public enum ErrorCode {
        INVALID_MESSAGE,
        MISSING_FIELD,
        INVALID_QUANTITY,
        UNKNOWN_PRODUCT,
        UNAUTHORIZED_OPERATION,
        INVALID_DELIVERY
    }

    public enum Product {
        coffee, tea, croissant
    }

    // Shared data structure
    public record Items(Integer coffee, Integer tea, Integer croissant) {}

    // 7 Reusable message structures
    public record EmptyMessage(MessageType type) {}
    public record ItemsMessage(MessageType type, Items items) {}
    public record AcceptedMessage(MessageType type, int orderId, int preparationTime) {}
    public record RefusedMessage(MessageType type, String reason) {}
    public record OrderIdMessage(MessageType type, int orderId) {}
    public record LowStockMessage(MessageType type, Product product, int remaining) {}
    public record ErrorMessage(MessageType type, ErrorCode code, String message) {}
}