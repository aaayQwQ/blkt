// BLKT JavaScript plugin API v1. Node.js modules and filesystem APIs are not provided.
BLKT.on("message", function (event) {
    BLKT.log(event.user + ": " + event.text);
});
BLKT.on("gift", function (event) {
    BLKT.log(event.user + " sent " + event.gift_name + " x" + event.count);
});
BLKT.log("example script initialized");
