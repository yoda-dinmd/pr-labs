import tools.jackson.databind.JsonNode;
import tools.jackson.databind.ObjectMapper;

import java.io.BufferedReader;
import java.io.IOException;

public class ServerListener implements Runnable {
    private final BufferedReader in;
    private final ObjectMapper mapper = new ObjectMapper();

    public ServerListener(BufferedReader in) {
        this.in = in;
    }

    @Override
    public void run() {
        try {
            String serverMessage;
            // Blocks here waiting for lines from the server, independent of your keyboard
            while ((serverMessage = in.readLine()) != null) {
                System.out.println("\n[SERVER]: " + serverMessage);
                try {
                    final JsonNode root = mapper.readTree(serverMessage);
                    JsonNode typeNode = root.path("type");
                    Protocol.MessageType type = (typeNode != null && typeNode.isString())
                            ? Protocol.MessageType.valueOf(typeNode.asString())
                            : null;

                    switch (type) {
                        case null -> System.out.println("Missing 'type' field");
                        case MENU_RESPONSE -> {
                            JsonNode itemsNode = root.path("items");
                            Protocol.Items items = new Protocol.Items(
                                    itemsNode.path("coffee").asInt(),
                                    itemsNode.path("tea").asInt(),
                                    itemsNode.path("croissant").asInt()
                            );

                            System.out.println("Menu of the day");
                            System.out.println("------------------");
                            System.out.println("Coffee: " + items.coffee());
                            System.out.println("Tea: " + items.tea());
                            System.out.println("Croissants: " + items.croissant());

                        }
                        case ORDER_ACCEPTED -> {
                            JsonNode orderIdNode = root.path("orderId");
                            JsonNode preparationTimeNode = root.path("preparationTime");
                            System.out.println("Order accepted! Your order number: " + orderIdNode.asInt());
                            System.out.println("ETA: " + preparationTimeNode.asInt() + " seconds");
                        }
                        case ORDER_REFUSED -> {
                            JsonNode reasonNode = root.path("reason");
                            System.out.println("Order refused. Reason: " + reasonNode.asString());
                        }
                        case ORDER_READY -> {
                            JsonNode orderIdNode = root.path("orderId");
                            System.out.println("Your order is ready! Order number: " + orderIdNode.asInt());
                        }
                        case LOW_STOCK -> {
                            JsonNode productNode = root.path("product");
                            JsonNode remainingNode = root.path("remaining");
                            System.out.println("Notice: Low " + productNode.asString() + " stock!");
                            System.out.println(remainingNode.asInt() + " left");
                        }
                        case ERROR -> {
                            String code = root.path("code").asString("UNKNOWN_ERROR");
                            String message = root.path("message").asString("No details provided.");
                            System.out.println("[ERROR - " + code + "]: " + message);
                        }
                        default -> System.out.println("Ignored invalid message type: " + type);
                    }
                } catch (Exception e) {
                    System.out.println("Invalid server message: " + e.getMessage());
                }
            }

            // readLine() returns null when the server closes the connection
            System.out.println("Server disconnected.");
            System.exit(0);
        } catch (IOException e) {
            System.out.println("Connection lost: " + e.getMessage());
            System.exit(0);
        }
    }
}
