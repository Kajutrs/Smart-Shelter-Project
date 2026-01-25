package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
)

// მონაცემების სტრუქტურა, რომელიც მოდის STM32-დან
type ShelterData struct {
	Temp     float64 `json:"temp"`
	Distance int     `json:"distance"`
	Heating  string  `json:"heating"`
}

func updateHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "მხოლოდ POST მოთხოვნებია დაშვებული", http.StatusMethodNotAllowed)
		return
	}

	var data ShelterData
	err := json.NewDecoder(r.Body).Decode(&data)
	if err != nil {
		http.Error(w, "მონაცემების წაკითხვის შეცდომა", http.StatusBadRequest)
		return
	}

	// მონაცემების ბეჭდვა კონსოლში
	fmt.Printf("მიღებულია მონაცემები -> ტემპერატურა: %.1f°C | მანძილი: %d მმ | გათბობა: %s\n", 
		data.Temp, data.Distance, data.Heating)
}

func main() {
	http.HandleFunc("/update", updateHandler)
	fmt.Println("ჭკვიანი თავშესაფრის სერვერი აქტიურია 8080 პორტზე...")
	log.Fatal(http.ListenAndServe(":8080", nil))
}
