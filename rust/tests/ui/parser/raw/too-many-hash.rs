// ignore-tidy-linelength

fn main() {
    let s: &str = r################################################################################################################################################################################################################################################################"very raw"################################################################################################################################################################################################################################################################;
    //~^ ERROR too many `#` symbols: raw strings may be delimited by up to 255 `#` symbols, but found 256
}
