# Coffee Shop — Final JSON Protocol

## 1. General structure

Every message is a JSON object containing a `type` field.

The `type` determines which additional fields are required. Each message has its own structure, but messages reuse common data structures wherever possible.

All messages are UTF-8 encoded and separated by `\n` over TCP.

## 2. Shared data structure

### `Items`

Used for placing orders, delivering supplies, and reporting stock.

```json
{
  "coffee": 2,
  "tea": 1,
  "croissant": 3
}
```

| Field | Type | Rules |
|---|---|---|
| `coffee` | Integer | ≥ 0 |
| `tea` | Integer | ≥ 0 |
| `croissant` | Integer | ≥ 0 |

All fields are optional. Missing fields mean `0`.

For orders and deliveries, at least one quantity must be greater than zero. For stock reports, all three fields must be included.

## 3. Client → Server

### `MENU_REQUEST`

Requests current stock.

```json
{
  "type": "MENU_REQUEST"
}
```

### `ORDER_REQUEST`

Places an order.

```json
{
  "type": "ORDER_REQUEST",
  "items": {
    "coffee": 2,
    "tea": 1
  }
}
```

Fields: `type`, `items: Items`.

## 4. Server → Client

### `MENU_RESPONSE`

Returns current stock.

```json
{
  "type": "MENU_RESPONSE",
  "items": {
    "coffee": 20,
    "tea": 15,
    "croissant": 10
  }
}
```

Fields: `type`, `items: Items`.

### `ORDER_ACCEPTED`

Confirms that the order was accepted.

```json
{
  "type": "ORDER_ACCEPTED",
  "orderId": 42,
  "preparationTime": 11
}
```

| Field | Type | Meaning |
|---|---|---|
| `orderId` | Integer | Positive, unique ID assigned by server |
| `preparationTime` | Integer | Estimated preparation time in seconds |

### `ORDER_REFUSED`

Returned when a valid order cannot be fulfilled due to insufficient stock.

```json
{
  "type": "ORDER_REFUSED",
  "reason": "Insufficient coffee"
}
```

Field: `reason: String`.

### `ORDER_READY`

Sent automatically when preparation finishes.

```json
{
  "type": "ORDER_READY",
  "orderId": 42
}
```

Field: `orderId: Integer`.

### `ERROR`

Returned for invalid requests, malformed data, or unauthorized operations.

```json
{
  "type": "ERROR",
  "code": "INVALID_QUANTITY",
  "message": "Quantity cannot be negative"
}
```

| Field | Type | Allowed values |
|---|---|---|
| `code` | Enum | See below |
| `message` | String | Explanation of the error |

Allowed error codes:

| Code | When used |
|---|---|
| `INVALID_MESSAGE` | Invalid JSON, invalid type, or incorrect structure |
| `MISSING_FIELD` | Required field is missing |
| `INVALID_QUANTITY` | Negative, non-integer, or otherwise invalid quantity |
| `UNKNOWN_PRODUCT` | Product other than coffee, tea, croissant |
| `UNAUTHORIZED_OPERATION` | Client attempts a provider-only operation |
| `INVALID_DELIVERY` | Invalid delivery contents |

## 5. Provider → Server

### `PROVIDER_IDENTIFY`

Sent once after connecting.

```json
{
  "type": "PROVIDER_IDENTIFY"
}
```

The server marks this connection as a provider connection. This is role identification, not secure authentication.

### `DELIVERY`

Increases available stock.

```json
{
  "type": "DELIVERY",
  "items": {
    "coffee": 20,
    "croissant": 10
  }
}
```

Fields: `type`, `items: Items`.

Only identified provider connections can send this message.

## 6. Server → Provider

### `LOW_STOCK`

Sent automatically when a product's quantity drops below 5.

```json
{
  "type": "LOW_STOCK",
  "product": "tea",
  "remaining": 4
}
```

| Field | Type | Allowed values |
|---|---|---|
| `product` | Enum | `coffee`, `tea`, `croissant` |
| `remaining` | Integer | 0–4 |

The provider may also receive the same `ERROR` message as clients.

---

## 7. Data structures to implement

You do **not** need a separate class for each message.

Use these reusable structures:

| Structure | Fields | Used by |
|---|---|---|
| `EmptyMessage` | `type` | `MENU_REQUEST`, `PROVIDER_IDENTIFY` |
| `ItemsMessage` | `type`, `items: Items` | `ORDER_REQUEST`, `MENU_RESPONSE`, `DELIVERY` |
| `AcceptedMessage` | `type`, `orderId`, `preparationTime` | `ORDER_ACCEPTED` |
| `RefusedMessage` | `type`, `reason` | `ORDER_REFUSED` |
| `OrderIdMessage` | `type`, `orderId` | `ORDER_READY` |
| `LowStockMessage` | `type`, `product`, `remaining` | `LOW_STOCK` |
| `ErrorMessage` | `type`, `code`, `message` | `ERROR` |

Plus the shared `Items` structure.

**Total: 8 data structures, 10 message types.**

The `type` enum is:

```text
MENU_REQUEST
MENU_RESPONSE
ORDER_REQUEST
ORDER_ACCEPTED
ORDER_REFUSED
ORDER_READY
PROVIDER_IDENTIFY
DELIVERY
LOW_STOCK
ERROR
```

## 8. Protocol rules

1. The receiver reads the `type` field first and deserializes the remaining JSON into the corresponding structure.
2. Any message containing a field of the wrong type, an unknown field, or an invalid enum value is rejected with `ERROR`.
3. Order quantities must be non-negative integers with at least one positive quantity.
4. Missing product quantities mean zero.
5. Insufficient stock results in `ORDER_REFUSED`; invalid requests result in `ERROR`.
6. The server generates order IDs. Clients never assign them.
7. `ORDER_READY` is sent asynchronously, using the ID assigned during acceptance.
8. The server associates orders with TCP connections. When a client disconnects, its unfinished orders are cancelled and their reserved stock restored.
9. The provider identifies itself once per TCP connection. A client connection cannot perform deliveries.
10. A provider disconnect does not stop the server.
11. Each JSON message ends with `\n`. Receivers must handle both partial messages and multiple messages arriving together.

---

## 9. Complete communication example

**CLIENT → SERVER**

```json
{"type":"MENU_REQUEST"}
```

**SERVER → CLIENT**

```json
{"type":"MENU_RESPONSE","items":{"coffee":20,"tea":15,"croissant":10}}
```

**CLIENT → SERVER**

```json
{"type":"ORDER_REQUEST","items":{"coffee":2,"tea":1}}
```

**SERVER → CLIENT**

```json
{"type":"ORDER_ACCEPTED","orderId":42,"preparationTime":11}
```

**SERVER → CLIENT (after preparation)**

```json
{"type":"ORDER_READY","orderId":42}
```

---

**Implementation recommendation:** Use one top-level message enum and the seven message structures plus `Items` shown above. Dispatch on `type` before deserialization into the corresponding structure.
