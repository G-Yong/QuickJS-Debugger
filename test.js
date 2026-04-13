function add(a, b) {
    var sum = a + b;
    console.log("sum =", sum);
    return sum;
    var unused = "This variable is never used";
}

var x = 10;
var y = 20;
var result = add(x, y);
console.log("result =", result);

for (var i = 0; i < 3; i++) {
    console.log("i =", i);
}

console.log("done!");
