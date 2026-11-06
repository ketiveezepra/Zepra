// External script for devtools_console_input.html: each message says its own line.
var demoHelper = { name: "helper", values: [10, 20, 30] };
console.log("external log at line 3");
console.info("external info at line 4");
console.debug("external debug at line 5");
console.warn("external warn at line 6");
console.error("external error at line 7");

function demoTick(n) {
  console.log("tick " + n + " from demoTick (line 10)");
}
for (var i = 1; i <= 3; i++) demoTick(i);

document.getElementById("list").childNodes.forEach(function (li) {
  console.debug("list item " + li.textContent + " (line 15)");
});
