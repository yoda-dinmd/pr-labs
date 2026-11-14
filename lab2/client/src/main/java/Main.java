import tools.jackson.databind.ObjectMapper;

import java.io.*;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

public class Main {
    static void main(String[] args) throws IOException {
        if (args.length < 3) {
            System.out.println("Usage: java Main <host> <port> <client|provider>");
            return;
        }
        String hostName = args[0];
        int portNumber = Integer.parseInt(args[1]);
        String role = args[2];
        try (
                Socket clientSocket = new Socket(hostName, portNumber);
                BufferedReader stdIn = new BufferedReader(new InputStreamReader(System.in))
        ) {
            PrintWriter out = new PrintWriter(new OutputStreamWriter(clientSocket.getOutputStream(), StandardCharsets.UTF_8), true);
            BufferedReader in = new BufferedReader(new InputStreamReader(clientSocket.getInputStream(), StandardCharsets.UTF_8));
            String userInput;

            Thread listenerThread = new Thread(new ServerListener(in));
            listenerThread.setDaemon(true);
            listenerThread.start();
            ObjectMapper mapper = new ObjectMapper();
            if (role.equalsIgnoreCase("provider")) {
                String json = mapper.writeValueAsString(new Protocol.EmptyMessage(Protocol.MessageType.PROVIDER_IDENTIFY));
                out.println(json);
            }

            while ((userInput = stdIn.readLine()) != null) {
                switch (userInput.trim().toLowerCase()) {
                    case "menu" -> {
                        String json = mapper.writeValueAsString(new Protocol.EmptyMessage(Protocol.MessageType.MENU_REQUEST));
                        out.println(json);
                    }
                    case "order", "deliver" -> {
                        Protocol.MessageType type = userInput.trim().equalsIgnoreCase("order")
                                ? Protocol.MessageType.ORDER_REQUEST
                                : Protocol.MessageType.DELIVERY;

                        int coffee = readAmount(stdIn, "Coffee: ");
                        int tea = readAmount(stdIn, "Tea: ");
                        int croissant = readAmount(stdIn, "Croissant: ");

                        Protocol.Items items = new Protocol.Items(coffee, tea, croissant);

                        if ((coffee + tea + croissant) == 0) {
                            System.out.println("Order must contain at least one item!");
                            break;
                        }

                        String json = mapper.writeValueAsString(new Protocol.ItemsMessage(type, items));
                        out.println(json);
                    }
                    case "raw" -> {
                        System.out.print("Raw payload: ");
                        out.println(stdIn.readLine());
                    }
                    case "exit" -> {
                        return;
                    }
                    default -> System.out.println("Unknown command. Use: menu," + (role.equalsIgnoreCase("client") ? " order" : " deliver") + ", raw, exit");
                }
            }
        }

    }
    private static int readAmount(BufferedReader stdIn, String label) throws IOException {
        while (true) {
            System.out.print(label);
            String line = stdIn.readLine();
            if (line == null) return 0;
            try {
                int val = Integer.parseInt(line.trim());
                if (val >= 0) return val;
                System.out.println("Amount cannot be negative.");
            } catch (NumberFormatException e) {
                System.out.println("Please enter a valid integer.");
            }
        }
    }
}
