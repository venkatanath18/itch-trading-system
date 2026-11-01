function(add_app name)
  add_executable(${name} apps/${name}.cpp)
  target_link_libraries(${name} PRIVATE core)
endfunction()

add_app(day_summary)
add_app(book_check)
add_app(book_diff)
add_app(signals_export)
add_app(calibrate)
add_app(trade)
add_app(mold_replay)
add_app(mold_receive)

# The same program with a counting operator new: fails if the day's run
# allocates. Not in sanitizer builds, which bring their own operator new.
if(NOT SANITIZE)
  add_executable(trade_alloc_check apps/trade.cpp apps/alloc_counter.cpp)
  target_link_libraries(trade_alloc_check PRIVATE core)
  target_compile_definitions(trade_alloc_check PRIVATE COUNT_ALLOCATIONS)
endif()
